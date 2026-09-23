// speed.on / speed.off, console, timers and the mod's own metadata.
#include "Api.h"

#include "JsRuntime.h"
#include "core/Config.h"
#include "core/Log.h"
#include "ui/CefHost.h"

#include <string>

namespace
{
    // speed.key(vk) - is that key held right now?
    //
    // The "keydown" event reports presses; a launch limiter needs the opposite
    // question, asked every frame: is the button still down. Reading the
    // keyboard state directly also keeps working while the game has focus,
    // which is where this is used.
    JSValue KeyDown(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.key(virtualKey)");

        int32_t vk = 0;
        if (JS_ToInt32(ctx, &vk, argv[0])) return JS_EXCEPTION;
        return JS_NewBool(ctx, (GetAsyncKeyState(vk) & 0x8000) != 0);
    }

    std::string Join(JSContext* ctx, int argc, JSValueConst* argv)
    {
        std::string line;
        for (int i = 0; i < argc; i++)
        {
            const char* s = JS_ToCString(ctx, argv[i]);
            if (!s) continue;
            if (i) line += " ";
            line += s;
            JS_FreeCString(ctx, s);
        }
        return line;
    }

    // O console na tela ja existe: e o mesmo painel de speed.print, no canal
    // "sl:log", com barra de comando e tudo. console.log so nao chegava nele.
    //
    // Passa pelo JSON.stringify do proprio QuickJS, como o Print faz, porque
    // aspas e acentos precisam atravessar inteiros - e escapar isso na mao,
    // numa funcao chamada dezenas de vezes por segundo por um mod em
    // desenvolvimento, e errado duas vezes.
    void Echo(JSContext* ctx, const char* id, const std::string& text)
    {
        static const bool on = Config::GetBool("UI", "Console", true);
        if (!on) return;

        JSValue payload = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, payload, "mod", JS_NewString(ctx, id ? id : "?"));
        JS_SetPropertyStr(ctx, payload, "text", JS_NewString(ctx, text.c_str()));

        JSValue json = JS_JSONStringify(ctx, payload, JS_UNDEFINED, JS_UNDEFINED);
        const char* enc = JS_ToCString(ctx, json);
        if (enc) { CefHost::SendToUi("sl:log", enc); JS_FreeCString(ctx, enc); }
        JS_FreeValue(ctx, json);
        JS_FreeValue(ctx, payload);
    }

    JSValue ConsoleLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        const char* id = mod ? mod->id.c_str() : "?";
        std::string line = Join(ctx, argc, argv);
        LogJs("[%s] %s", id, line.c_str());
        Echo(ctx, id, line);
        return JS_UNDEFINED;
    }

    JSValue ConsoleError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        const char* id = mod ? mod->id.c_str() : "?";
        std::string line = Join(ctx, argc, argv);
        LogJs("[%s] ERROR %s", id, line.c_str());
        // O painel nao tem nivel; a cor vem das marcacoes de speed.print.
        Echo(ctx, id, "{vermelho}ERRO{/} " + line);
        return JS_UNDEFINED;
    }

    JSValue On(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 2 || !JS_IsFunction(ctx, argv[1]))
            return JS_ThrowTypeError(ctx, "speed.on(event, fn)");

        const char* event = JS_ToCString(ctx, argv[0]);
        if (!event) return JS_EXCEPTION;

        JSValue list = JS_GetPropertyStr(ctx, mod->handlers, event);
        if (!JS_IsObject(list))
        {
            JS_FreeValue(ctx, list);
            list = JS_NewArray(ctx);
            JS_SetPropertyStr(ctx, mod->handlers, event, JS_DupValue(ctx, list));
        }

        JSValue lenVal = JS_GetPropertyStr(ctx, list, "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        JS_SetPropertyUint32(ctx, list, len, JS_DupValue(ctx, argv[1]));

        JS_FreeValue(ctx, list);
        JS_FreeCString(ctx, event);
        return JS_UNDEFINED;
    }

    // speed.emit("fuel:state", { litros: 12 }) - to every OTHER mod's
    // speed.on("fuel:state"), next frame. The name needs a colon, so it cannot
    // pass for one of the loader's own events ("frame", "keydown", ...).
    JSValue EmitEvent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1) return JS_ThrowTypeError(ctx, "speed.emit(event, data?)");

        const char* event = JS_ToCString(ctx, argv[0]);
        if (!event) return JS_EXCEPTION;
        std::string name = event;
        JS_FreeCString(ctx, event);
        if (name.find(':') == std::string::npos || name.compare(0, 3, "ui:") == 0)
            return JS_ThrowTypeError(ctx, "speed.emit: the event needs a colon, as in \"fuel:state\"");

        std::string json;
        if (argc >= 2 && !JS_IsUndefined(argv[1]))
        {
            JSValue text = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(text)) return JS_EXCEPTION;
            const char* enc = JS_ToCString(ctx, text);
            if (enc) { json = enc; JS_FreeCString(ctx, enc); }
            JS_FreeValue(ctx, text);
        }
        Js::Post(mod, name, json);
        return JS_UNDEFINED;
    }

    JSValue Off(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1) return JS_UNDEFINED;

        const char* event = JS_ToCString(ctx, argv[0]);
        if (!event) return JS_EXCEPTION;

        if (argc < 2)
        {
            // No function given: drop every listener for that event.
            JS_SetPropertyStr(ctx, mod->handlers, event, JS_NewArray(ctx));
            JS_FreeCString(ctx, event);
            return JS_UNDEFINED;
        }

        JSValue list = JS_GetPropertyStr(ctx, mod->handlers, event);
        if (JS_IsObject(list))
        {
            JSValue splice = JS_GetPropertyStr(ctx, list, "splice");
            JSValue indexOf = JS_GetPropertyStr(ctx, list, "indexOf");
            JSValue idx = JS_Call(ctx, indexOf, list, 1, &argv[1]);
            int32_t i = -1;
            JS_ToInt32(ctx, &i, idx);
            if (i >= 0)
            {
                JSValue args[2] = { JS_NewInt32(ctx, i), JS_NewInt32(ctx, 1) };
                JS_FreeValue(ctx, JS_Call(ctx, splice, list, 2, args));
                JS_FreeValue(ctx, args[0]);
                JS_FreeValue(ctx, args[1]);
            }
            JS_FreeValue(ctx, idx);
            JS_FreeValue(ctx, indexOf);
            JS_FreeValue(ctx, splice);
        }
        JS_FreeValue(ctx, list);
        JS_FreeCString(ctx, event);
        return JS_UNDEFINED;
    }

    JSValue AddTimer(JSContext* ctx, int argc, JSValueConst* argv, bool repeat)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1 || !JS_IsFunction(ctx, argv[0]))
            return JS_ThrowTypeError(ctx, "a function is expected");

        int32_t delay = 0;
        if (argc >= 2) JS_ToInt32(ctx, &delay, argv[1]);
        if (delay < 0) delay = 0;

        Js::Timer t;
        t.id = mod->nextTimerId++;
        t.due = GetTickCount() + delay;
        t.interval = repeat ? (delay > 0 ? delay : 1) : 0;
        t.fn = JS_DupValue(ctx, argv[0]);
        t.dead = false;
        mod->timers.push_back(t);
        return JS_NewInt32(ctx, t.id);
    }

    JSValue SetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        return AddTimer(ctx, argc, argv, false);
    }

    JSValue SetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        return AddTimer(ctx, argc, argv, true);
    }

    JSValue ClearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1) return JS_UNDEFINED;

        int32_t id = 0;
        JS_ToInt32(ctx, &id, argv[0]);
        for (size_t i = 0; i < mod->timers.size(); i++)
            if (mod->timers[i].id == id) mod->timers[i].dead = true;
        return JS_UNDEFINED;
    }

    // Everything SpeedLoader has loaded, in load order. A mod calling this from
    // the top level of its own main.js sees a partial list - it is pushed before
    // its entry point runs, and whoever comes after is not there yet. From a
    // frame or an event, the list is complete.
    JSValue ModList(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        const std::vector<Js::Mod*>& mods = Js::Mods();
        JSValue arr = JS_NewArray(ctx);

        for (uint32_t i = 0; i < (uint32_t)mods.size(); i++)
        {
            JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "id",
                              JS_NewString(ctx, mods[i]->id.c_str()));
            JS_SetPropertyStr(ctx, entry, "name",
                              JS_NewString(ctx, mods[i]->name.c_str()));
            JS_SetPropertyStr(ctx, entry, "ui",
                              mods[i]->ui.empty() ? JS_FALSE : JS_TRUE);
            JS_SetPropertyUint32(ctx, arr, i, entry);
        }
        return arr;
    }

    JSValue Now(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        return JS_NewInt64(ctx, (int64_t)GetTickCount());
    }
}

namespace Js
{
    void RegisterCore(JSContext* ctx, JSValue speed, Mod* mod)
    {
        JSValue global = JS_GetGlobalObject(ctx);

        JS_SetPropertyStr(ctx, speed, "key",
                          JS_NewCFunction(ctx, KeyDown, "key", 1));

        JSValue console = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, console, "log",
                          JS_NewCFunction(ctx, ConsoleLog, "log", 1));
        JS_SetPropertyStr(ctx, console, "info",
                          JS_NewCFunction(ctx, ConsoleLog, "info", 1));
        JS_SetPropertyStr(ctx, console, "warn",
                          JS_NewCFunction(ctx, ConsoleError, "warn", 1));
        JS_SetPropertyStr(ctx, console, "error",
                          JS_NewCFunction(ctx, ConsoleError, "error", 1));
        JS_SetPropertyStr(ctx, global, "console", console);

        JS_SetPropertyStr(ctx, global, "setTimeout",
                          JS_NewCFunction(ctx, SetTimeout, "setTimeout", 2));
        JS_SetPropertyStr(ctx, global, "setInterval",
                          JS_NewCFunction(ctx, SetInterval, "setInterval", 2));
        JS_SetPropertyStr(ctx, global, "clearTimeout",
                          JS_NewCFunction(ctx, ClearTimer, "clearTimeout", 1));
        JS_SetPropertyStr(ctx, global, "clearInterval",
                          JS_NewCFunction(ctx, ClearTimer, "clearInterval", 1));
        JS_FreeValue(ctx, global);

        JS_SetPropertyStr(ctx, speed, "version", JS_NewString(ctx, "0.1.0"));
        JS_SetPropertyStr(ctx, speed, "on",  JS_NewCFunction(ctx, On, "on", 2));
        JS_SetPropertyStr(ctx, speed, "off", JS_NewCFunction(ctx, Off, "off", 2));
        JS_SetPropertyStr(ctx, speed, "emit", JS_NewCFunction(ctx, EmitEvent, "emit", 2));
        JS_SetPropertyStr(ctx, speed, "now", JS_NewCFunction(ctx, Now, "now", 0));
        JS_SetPropertyStr(ctx, speed, "mods", JS_NewCFunction(ctx, ModList, "mods", 0));

        JSValue info = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, info, "id",   JS_NewString(ctx, mod->id.c_str()));
        JS_SetPropertyStr(ctx, info, "name", JS_NewString(ctx, mod->name.c_str()));
        JS_SetPropertyStr(ctx, info, "dir",  JS_NewString(ctx, mod->dir.c_str()));
        JS_SetPropertyStr(ctx, speed, "mod", info);
    }
}
