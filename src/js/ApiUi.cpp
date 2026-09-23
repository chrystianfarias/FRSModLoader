// speed.ui - the HTML layer as seen from the mod side.
//
// A mod never touches the DOM from here: it sends data (speed.ui.send) and
// listens to what the page answers (speed.on("ui:<channel>", ...)). Drawing is
// the HTML's job.
#include "Api.h"

#include "ui/CefHost.h"
#include "ui/InputRouter.h"
#include "ui/Overlay.h"

#include <string>

namespace
{
    std::string Stringify(JSContext* ctx, JSValueConst v)
    {
        if (JS_IsUndefined(v)) return "null";

        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(json)) { JS_FreeValue(ctx, json); return "null"; }

        const char* s = JS_ToCString(ctx, json);
        std::string out = s ? s : "null";
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, json);
        return out;
    }

    JSValue Send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1)
            return JS_ThrowTypeError(ctx, "speed.ui.send(channel, data)");

        const char* channel = JS_ToCString(ctx, argv[0]);
        if (!channel) return JS_EXCEPTION;

        // The channel is prefixed with the mod id: there is a single page,
        // and it needs to know who sent this to route it to the right piece.
        CefHost::SendToUi(mod->id + ":" + channel,
                          argc >= 2 ? Stringify(ctx, argv[1]) : "null");
        JS_FreeCString(ctx, channel);
        return JS_UNDEFINED;
    }

    JSValue Show(JSContext*, JSValueConst, int, JSValueConst*)
    { Overlay::SetVisible(true);  return JS_UNDEFINED; }

    JSValue Hide(JSContext*, JSValueConst, int, JSValueConst*)
    { Overlay::SetVisible(false); InputRouter::SetCapturing(false); return JS_UNDEFINED; }

    JSValue Toggle(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        bool visible = !Overlay::IsVisible();
        Overlay::SetVisible(visible);
        if (!visible) InputRouter::SetCapturing(false);
        return visible ? JS_TRUE : JS_FALSE;
    }

    JSValue Visible(JSContext*, JSValueConst, int, JSValueConst*)
    { return Overlay::IsVisible() ? JS_TRUE : JS_FALSE; }

    JSValue Capture(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        bool on = argc < 1 || JS_ToBool(ctx, argv[0]) != 0;
        // speed.ui.capture(true, { mouse: false }): the keyboard only.
        bool mouse = true;
        if (argc >= 2 && JS_IsObject(argv[1]))
        {
            JSValue m = JS_GetPropertyStr(ctx, argv[1], "mouse");
            if (!JS_IsUndefined(m)) mouse = JS_ToBool(ctx, m) != 0;
            JS_FreeValue(ctx, m);
        }
        InputRouter::SetCapturing(on, mouse);
        return JS_UNDEFINED;
    }

    JSValue Capturing(JSContext*, JSValueConst, int, JSValueConst*)
    { return InputRouter::IsCapturing() ? JS_TRUE : JS_FALSE; }

    JSValue Reload(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        bool hard = argc >= 1 && JS_ToBool(ctx, argv[0]) != 0;
        CefHost::Reload(hard);
        return JS_UNDEFINED;
    }

    JSValue DevTools(JSContext*, JSValueConst, int, JSValueConst*)
    { CefHost::ShowDevTools(); return JS_UNDEFINED; }

    JSValue Size(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        int w = 0, h = 0;
        Overlay::Size(&w, &h);
        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "width",  JS_NewInt32(ctx, w));
        JS_SetPropertyStr(ctx, out, "height", JS_NewInt32(ctx, h));
        return out;
    }
}

namespace Js
{
    void RegisterUi(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue ui = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, ui, "send",      JS_NewCFunction(ctx, Send, "send", 2));
        JS_SetPropertyStr(ctx, ui, "show",      JS_NewCFunction(ctx, Show, "show", 0));
        JS_SetPropertyStr(ctx, ui, "hide",      JS_NewCFunction(ctx, Hide, "hide", 0));
        JS_SetPropertyStr(ctx, ui, "toggle",    JS_NewCFunction(ctx, Toggle, "toggle", 0));
        JS_SetPropertyStr(ctx, ui, "visible",   JS_NewCFunction(ctx, Visible, "visible", 0));
        JS_SetPropertyStr(ctx, ui, "capture",   JS_NewCFunction(ctx, Capture, "capture", 1));
        JS_SetPropertyStr(ctx, ui, "capturing", JS_NewCFunction(ctx, Capturing, "capturing", 0));
        JS_SetPropertyStr(ctx, ui, "reload",    JS_NewCFunction(ctx, Reload, "reload", 1));
        JS_SetPropertyStr(ctx, ui, "devtools",  JS_NewCFunction(ctx, DevTools, "devtools", 0));
        JS_SetPropertyStr(ctx, ui, "size",      JS_NewCFunction(ctx, Size, "size", 0));

        JS_SetPropertyStr(ctx, speed, "ui", ui);
    }
}
