#pragma once

#include "include/cef_app.h"
#include "include/cef_v8.h"
#include <string>

// The renderer side of the bridge. Runs in FRSModLoaderHelper.exe, not in the
// game.
//
// It injects `window.frsmodloader` into the page:
//
//   frsmodloader.send(channel, data)   -> reaches the mod's JS (QuickJS, in-game)
//   frsmodloader.on(channel, cb)       <- receives whatever the mod sends
//
// The return path is a process message that CefHost hands to the mod.

// Messages exchanged between renderer and browser. The payload is always a
// JSON string: it is the lowest common denominator between V8 and QuickJS, and
// it avoids marshalling types.
#define SL_MSG_TO_MOD "sl.toMod"   // renderer -> browser
#define SL_MSG_TO_UI  "sl.toUi"    // browser  -> renderer

class SlV8Send : public CefV8Handler
{
public:
    bool Execute(const CefString& name, CefRefPtr<CefV8Value>,
                 const CefV8ValueList& args, CefRefPtr<CefV8Value>&,
                 CefString&) override
    {
        if (name != "__frsmodloader_send" || args.size() < 2) return false;

        CefRefPtr<CefV8Context> ctx = CefV8Context::GetCurrentContext();
        CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create(SL_MSG_TO_MOD);
        msg->GetArgumentList()->SetString(0, args[0]->GetStringValue());
        msg->GetArgumentList()->SetString(1, args[1]->GetStringValue());
        ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
        return true;
    }

    IMPLEMENT_REFCOUNTING(SlV8Send);
};

class RenderApp : public CefApp, public CefRenderProcessHandler
{
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override
    {
        return this;
    }

    void OnContextCreated(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override
    {
        CefRefPtr<CefV8Value> global = context->GetGlobal();
        global->SetValue("__frsmodloader_send",
                         CefV8Value::CreateFunction("__frsmodloader_send",
                                                    new SlV8Send()),
                         V8_PROPERTY_ATTRIBUTE_DONTENUM);

        // The rest of the API is plain JS: less native code, and the surface
        // can grow without recompiling the helper.
        frame->ExecuteJavaScript(
            "(function(){"
            "  var handlers = {};"
            "  window.frsmodloader = {"
            "    send: function(channel, data){"
            "      __frsmodloader_send(String(channel), JSON.stringify(data === undefined ? null : data));"
            "    },"
            "    on: function(channel, cb){"
            "      (handlers[channel] || (handlers[channel] = [])).push(cb);"
            "      return function(){ frsmodloader.off(channel, cb); };"
            "    },"
            "    off: function(channel, cb){"
            "      var a = handlers[channel]; if(!a) return;"
            "      var i = a.indexOf(cb); if(i >= 0) a.splice(i, 1);"
            "    }"
            "  };"
            "  window.__frsmodloader_dispatch = function(channel, json){"
            "    var data; try { data = JSON.parse(json); } catch(e) { data = null; }"
            "    var a = handlers[channel] || [];"
            "    for (var i = 0; i < a.length; i++) {"
            "      try { a[i](data); } catch(e) { console.error('[frsmodloader]', e); }"
            "    }"
            "  };"
            "})();",
            frame->GetURL(), 0);
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                                  CefProcessId, CefRefPtr<CefProcessMessage> msg) override
    {
        if (msg->GetName() != SL_MSG_TO_UI) return false;

        CefRefPtr<CefListValue> a = msg->GetArgumentList();
        CefRefPtr<CefV8Context> ctx = frame->GetV8Context();
        if (!ctx || !ctx->Enter()) return true;

        CefRefPtr<CefV8Value> fn =
            ctx->GetGlobal()->GetValue("__frsmodloader_dispatch");
        if (fn && fn->IsFunction())
        {
            CefV8ValueList args;
            args.push_back(CefV8Value::CreateString(a->GetString(0)));
            args.push_back(CefV8Value::CreateString(a->GetString(1)));
            fn->ExecuteFunction(nullptr, args);
        }
        ctx->Exit();
        return true;
    }

    IMPLEMENT_REFCOUNTING(RenderApp);
};
