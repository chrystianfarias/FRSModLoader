// speed.settings - the mod's options, as the player set them in the Mods menu.
//
// A mod does not draw its settings; it lists them, in mod.json:
//
//   "settings": [
//     { "type": "section", "label": "Sound" },
//     { "id": "volume", "type": "range",  "label": "Volume", "min": 0, "max": 100,
//       "step": 5, "unit": "%", "default": 80 },
//     { "id": "pops",   "type": "toggle", "label": "Pops on lift-off", "default": true },
//     { "id": "engine", "type": "choice", "label": "Engine",
//       "options": [{ "value": "rb26", "label": "RB26" }, "2JZ"], "default": "rb26" },
//     { "id": "reset",  "type": "action", "label": "Reset the tank" }
//   ]
//
// and the loader's page (ui/mods.html) draws them in the game's own style. What
// the mod gets is values:
//
//   speed.settings.get("volume")          the value, or the default
//   speed.settings.all()                  every value, defaults filled in
//   speed.settings.set("volume", 60)      changed from the mod's side (a hotkey)
//   speed.settings.define([...])          more settings, decided at runtime
//   speed.on("settings", (all) => ...)    the player changed something
//   speed.on("settings:action", (e) => ...)   an "action" row: e.id
//
// The values are stored by the loader (host/ModSettings.cpp), not in the mod's
// folder, so an update of the mod keeps them.
#include "Api.h"
#include "JsRuntime.h"

#include "core/Log.h"
#include "host/ModSettings.h"

#include <map>
#include <string>

namespace
{
    struct State
    {
        JSValue schema;   // array of setting descriptions
        JSValue values;   // what was stored, without defaults
    };
    std::map<Js::Mod*, State> g_state;

    std::string ReadManifest(const std::string& dir)
    {
        std::string text;
        FILE* f = fopen((dir + "\\mod.json").c_str(), "rb");
        if (!f) return text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        return text;
    }

    JSValue ParseOr(JSContext* ctx, const std::string& text, JSValue fallback)
    {
        JSValue v = JS_ParseJSON(ctx, text.c_str(), text.size(), "<settings>");
        if (JS_IsException(v))
        {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return fallback;
        }
        JS_FreeValue(ctx, fallback);
        return v;
    }

    State& StateOf(Js::Mod* mod) { return g_state[mod]; }

    // Every value with its default underneath: the object a mod reads.
    JSValue All(JSContext* ctx, State& st)
    {
        JSValue out = JS_NewObject(ctx);

        uint32_t count = 0;
        JSValue len = JS_GetPropertyStr(ctx, st.schema, "length");
        JS_ToUint32(ctx, &count, len);
        JS_FreeValue(ctx, len);
        for (uint32_t i = 0; i < count; i++)
        {
            JSValue item = JS_GetPropertyUint32(ctx, st.schema, i);
            JSValue id = JS_GetPropertyStr(ctx, item, "id");
            JSValue def = JS_GetPropertyStr(ctx, item, "default");
            if (JS_IsString(id) && !JS_IsUndefined(def))
            {
                const char* key = JS_ToCString(ctx, id);
                if (key) { JS_SetPropertyStr(ctx, out, key, def); def = JS_UNDEFINED; }
                JS_FreeCString(ctx, key);
            }
            JS_FreeValue(ctx, def);
            JS_FreeValue(ctx, id);
            JS_FreeValue(ctx, item);
        }

        JSPropertyEnum* props = 0;
        uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &n, st.values,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0)
        {
            for (uint32_t i = 0; i < n; i++)
                JS_SetProperty(ctx, out, props[i].atom,
                               JS_GetProperty(ctx, st.values, props[i].atom));
            JS_FreePropertyEnum(ctx, props, n);
        }
        return out;
    }

    std::string Stringify(JSContext* ctx, JSValue v)
    {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        std::string out;
        if (const char* s = JS_ToCString(ctx, json)) { out = s; JS_FreeCString(ctx, s); }
        JS_FreeValue(ctx, json);
        return out;
    }

    JSValue Get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1) return JS_ThrowTypeError(ctx, "speed.settings.get(id, fallback?)");

        const char* key = JS_ToCString(ctx, argv[0]);
        if (!key) return JS_EXCEPTION;
        JSValue all = All(ctx, StateOf(mod));
        JSValue v = JS_GetPropertyStr(ctx, all, key);
        JS_FreeValue(ctx, all);
        JS_FreeCString(ctx, key);

        if (JS_IsUndefined(v) && argc >= 2) return JS_DupValue(ctx, argv[1]);
        return v;
    }

    JSValue AllFn(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        Js::Mod* mod = Js::Owner(ctx);
        return mod ? All(ctx, StateOf(mod)) : JS_NewObject(ctx);
    }

    JSValue Set(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 2) return JS_ThrowTypeError(ctx, "speed.settings.set(id, value)");

        const char* key = JS_ToCString(ctx, argv[0]);
        if (!key) return JS_EXCEPTION;
        State& st = StateOf(mod);
        JS_SetPropertyStr(ctx, st.values, key, JS_DupValue(ctx, argv[1]));
        JS_FreeCString(ctx, key);

        // Stored, not announced: the mod that set it already knows.
        ModSettings::SetValues(mod->id, Stringify(ctx, st.values), false);
        return JS_UNDEFINED;
    }

    JSValue Define(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        Js::Mod* mod = Js::Owner(ctx);
        if (!mod || argc < 1 || !JS_IsArray(argv[0]))
            return JS_ThrowTypeError(ctx, "speed.settings.define([ { id, type, label, ... } ])");

        State& st = StateOf(mod);
        uint32_t have = 0, count = 0;
        JSValue len = JS_GetPropertyStr(ctx, st.schema, "length");
        JS_ToUint32(ctx, &have, len);
        JS_FreeValue(ctx, len);
        len = JS_GetPropertyStr(ctx, argv[0], "length");
        JS_ToUint32(ctx, &count, len);
        JS_FreeValue(ctx, len);
        for (uint32_t i = 0; i < count; i++)
            JS_SetPropertyUint32(ctx, st.schema, have + i, JS_GetPropertyUint32(ctx, argv[0], i));

        ModSettings::Define(mod->id, Stringify(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    Js::Mod* ById(const std::string& id)
    {
        const std::vector<Js::Mod*>& mods = Js::Mods();
        for (size_t i = 0; i < mods.size(); i++)
            if (mods[i]->id == id) return mods[i];
        return 0;
    }
}

namespace Js
{
    void RegisterSettings(JSContext* ctx, JSValue speed, Mod* mod)
    {
        State& st = StateOf(mod);
        st.schema = JS_NewArray(ctx);
        st.values = JS_NewObject(ctx);

        JSValue meta = ParseOr(ctx, ReadManifest(mod->dir), JS_UNDEFINED);
        if (JS_IsObject(meta))
        {
            JSValue list = JS_GetPropertyStr(ctx, meta, "settings");
            if (JS_IsArray(list)) { JS_FreeValue(ctx, st.schema); st.schema = list; }
            else JS_FreeValue(ctx, list);
        }
        JS_FreeValue(ctx, meta);

        st.values = ParseOr(ctx, ModSettings::Values(mod->id), st.values);
        if (!JS_IsObject(st.values)) { JS_FreeValue(ctx, st.values); st.values = JS_NewObject(ctx); }

        JSValue settings = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, settings, "get",    JS_NewCFunction(ctx, Get, "get", 2));
        JS_SetPropertyStr(ctx, settings, "all",    JS_NewCFunction(ctx, AllFn, "all", 0));
        JS_SetPropertyStr(ctx, settings, "set",    JS_NewCFunction(ctx, Set, "set", 2));
        JS_SetPropertyStr(ctx, settings, "define", JS_NewCFunction(ctx, Define, "define", 1));
        JS_SetPropertyStr(ctx, speed, "settings", settings);
    }

    void SettingsChanged(const std::string& id, const std::string& valuesJson)
    {
        Mod* mod = ById(id);
        if (!mod) return;   // not running: it reads the file when it starts

        State& st = StateOf(mod);
        JSValue values = ParseOr(mod->ctx, valuesJson, JS_UNDEFINED);
        if (!JS_IsObject(values)) { JS_FreeValue(mod->ctx, values); return; }
        JS_FreeValue(mod->ctx, st.values);
        st.values = values;

        JSValue all = All(mod->ctx, st);
        std::string json = Stringify(mod->ctx, all);
        JS_FreeValue(mod->ctx, all);
        EmitTo(mod, "settings", json);
    }

    void SettingsAction(const std::string& id, const std::string& actionJson)
    {
        if (Mod* mod = ById(id)) EmitTo(mod, "settings:action", actionJson);
    }
}
