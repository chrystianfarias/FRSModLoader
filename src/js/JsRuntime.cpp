#include "JsRuntime.h"

#include "Api.h"

#include "host/ModSettings.h"
#include "ui/InputRouter.h"
#include "core/Log.h"

#include <stdio.h>
#include <string>
#include <vector>

namespace
{
    JSRuntime* g_rt = nullptr;
    std::vector<Js::Mod*> g_mods;
    const std::string kEmpty;

    bool ReadFile(const std::string& path, std::string* out)
    {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        out->resize(size > 0 ? size : 0);
        size_t got = size > 0 ? fread(&(*out)[0], 1, size, f) : 0;
        out->resize(got);
        fclose(f);
        return true;
    }

    // Resolves `import "./x.js"` inside the mod's folder.
    JSModuleDef* ModuleLoader(JSContext* ctx, const char* moduleName, void*)
    {
        Js::Mod* mod = Js::Owner(ctx);
        std::string path = moduleName;
        if (mod && path.size() > 1 && path[1] != ':' && path[0] != '\\')
            path = mod->dir + "\\" + path;

        std::string source;
        if (!ReadFile(path, &source))
        {
            JS_ThrowReferenceError(ctx, "module not found: %s", moduleName);
            return nullptr;
        }

        JSValue val = JS_Eval(ctx, source.c_str(), source.size(), moduleName,
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(val))
        {
            Js::ReportException(ctx, moduleName);
            return nullptr;
        }

        JSModuleDef* m = (JSModuleDef*)JS_VALUE_GET_PTR(val);
        JS_FreeValue(ctx, val);
        return m;
    }

    void PumpJobs()
    {
        JSContext* ctx = nullptr;
        for (int i = 0; i < 64; i++)
        {
            int rc = JS_ExecutePendingJob(g_rt, &ctx);
            if (rc <= 0)
            {
                if (rc < 0 && ctx) Js::ReportException(ctx, "promise");
                break;
            }
        }
    }

    void RunTimers(Js::Mod* mod)
    {
        if (mod->timers.empty()) return;
        DWORD now = GetTickCount();

        // Collect the due ones before calling anything: a callback may create
        // or kill timers, and mutating the vector mid-iteration is a trap.
        std::vector<int> due;
        for (size_t i = 0; i < mod->timers.size(); i++)
        {
            Js::Timer& t = mod->timers[i];
            if (!t.dead && (int)(now - t.due) >= 0) due.push_back(t.id);
        }

        for (size_t i = 0; i < due.size(); i++)
        {
            Js::Timer* t = nullptr;
            for (size_t k = 0; k < mod->timers.size(); k++)
                if (mod->timers[k].id == due[i] && !mod->timers[k].dead)
                    t = &mod->timers[k];
            if (!t) continue;

            JSValue fn = JS_DupValue(mod->ctx, t->fn);
            bool repeat = t->interval > 0;
            if (repeat) t->due = now + t->interval;
            else        t->dead = true;

            JSValue r = JS_Call(mod->ctx, fn, JS_UNDEFINED, 0, nullptr);
            if (JS_IsException(r)) Js::ReportException(mod->ctx, "timer");
            JS_FreeValue(mod->ctx, r);
            JS_FreeValue(mod->ctx, fn);
        }

        for (size_t i = mod->timers.size(); i > 0; i--)
        {
            Js::Timer& t = mod->timers[i - 1];
            if (t.dead)
            {
                JS_FreeValue(mod->ctx, t.fn);
                mod->timers.erase(mod->timers.begin() + (i - 1));
            }
        }
    }

    void CallHandlers(Js::Mod* mod, const char* event, JSValue arg)
    {
        JSValue list = JS_GetPropertyStr(mod->ctx, mod->handlers, event);
        if (!JS_IsArray(list)) { JS_FreeValue(mod->ctx, list); return; }

        JSValue lenVal = JS_GetPropertyStr(mod->ctx, list, "length");
        uint32_t len = 0;
        JS_ToUint32(mod->ctx, &len, lenVal);
        JS_FreeValue(mod->ctx, lenVal);

        for (uint32_t i = 0; i < len; i++)
        {
            JSValue fn = JS_GetPropertyUint32(mod->ctx, list, i);
            if (JS_IsFunction(mod->ctx, fn))
            {
                JSValue argv[1] = { arg };
                JSValue r = JS_Call(mod->ctx, fn, JS_UNDEFINED, 1, argv);
                if (JS_IsException(r)) Js::ReportException(mod->ctx, event);
                JS_FreeValue(mod->ctx, r);
            }
            JS_FreeValue(mod->ctx, fn);
        }
        JS_FreeValue(mod->ctx, list);
    }

    std::string JsonField(JSContext* ctx, JSValue obj, const char* key,
                          const char* def)
    {
        JSValue v = JS_GetPropertyStr(ctx, obj, key);
        std::string out = def ? def : "";
        if (JS_IsString(v))
        {
            const char* s = JS_ToCString(ctx, v);
            if (s) { out = s; JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, v);
        return out;
    }
}

namespace Js
{
    Mod* Owner(JSContext* ctx) { return (Mod*)JS_GetContextOpaque(ctx); }

    void ReportException(JSContext* ctx, const char* what)
    {
        Mod* mod = Owner(ctx);
        JSValue e = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, e);
        LogJs("[%s] error in %s: %s", mod ? mod->id.c_str() : "?", what,
              msg ? msg : "(no message)");
        if (msg) JS_FreeCString(ctx, msg);

        JSValue stack = JS_GetPropertyStr(ctx, e, "stack");
        if (!JS_IsUndefined(stack))
        {
            const char* s = JS_ToCString(ctx, stack);
            if (s) { LogJs("  %s", s); JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, e);
    }

    bool Init()
    {
        g_rt = JS_NewRuntime();
        if (!g_rt) { LogJs("JS_NewRuntime FAILED"); return false; }

        // A mod should not be able to take the game down through memory use,
        // nor freeze the frame in an endless loop without leaving a trace.
        JS_SetMemoryLimit(g_rt, 128 * 1024 * 1024);
        JS_SetMaxStackSize(g_rt, 1024 * 1024);
        JS_SetModuleLoaderFunc(g_rt, nullptr, ModuleLoader, nullptr);
        LogJs("QuickJS ready");
        return true;
    }

    void Shutdown()
    {
        for (size_t i = 0; i < g_mods.size(); i++)
        {
            Mod* mod = g_mods[i];
            for (size_t k = 0; k < mod->timers.size(); k++)
                JS_FreeValue(mod->ctx, mod->timers[k].fn);
            JS_FreeValue(mod->ctx, mod->handlers);
            JS_FreeContext(mod->ctx);
            delete mod;
        }
        g_mods.clear();
        if (g_rt) { JS_FreeRuntime(g_rt); g_rt = nullptr; }
    }

    const std::vector<Mod*>& Mods()           { return g_mods; }
    const std::string& ModId(const Mod* m)    { return m ? m->id   : kEmpty; }
    const std::string& ModDir(const Mod* m)   { return m ? m->dir  : kEmpty; }
    const std::string& ModUi(const Mod* m)    { return m ? m->ui   : kEmpty; }
    const std::string& ModName(const Mod* m)  { return m ? m->name : kEmpty; }

    bool ParseManifest(const std::string& text, const std::string& fallback,
                       std::string* id)
    {
        if (!g_rt) return false;
        JSContext* ctx = JS_NewContext(g_rt);
        if (!ctx) return false;

        JSValue meta = JS_ParseJSON(ctx, text.c_str(), text.size(), "<manifest>");
        const bool ok = !JS_IsException(meta) && JS_IsObject(meta);
        if (ok) *id = JsonField(ctx, meta, "id", fallback.c_str());
        else    JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, meta);
        JS_FreeContext(ctx);
        return ok && !id->empty();
    }

    bool IsRunning(const std::string& id)
    {
        for (size_t i = 0; i < g_mods.size(); i++)
            if (g_mods[i]->id == id) return true;
        return false;
    }

    Mod* Load(const std::string& dir)
    {
        std::string manifestPath = dir + "\\mod.json";
        std::string manifest;
        if (!ReadFile(manifestPath, &manifest))
        {
            LogJs("no mod.json in %s", dir.c_str());
            return nullptr;
        }

        Mod* mod = new Mod();
        mod->dir = dir;
        mod->ctx = JS_NewContext(g_rt);
        if (!mod->ctx) { delete mod; return nullptr; }
        JS_SetContextOpaque(mod->ctx, mod);

        JSValue meta = JS_ParseJSON(mod->ctx, manifest.c_str(), manifest.size(),
                                    manifestPath.c_str());
        if (JS_IsException(meta))
        {
            ReportException(mod->ctx, "mod.json");
            JS_FreeContext(mod->ctx);
            delete mod;
            return nullptr;
        }

        size_t slash = dir.find_last_of('\\');
        std::string folder = slash == std::string::npos ? dir : dir.substr(slash + 1);

        mod->id   = JsonField(mod->ctx, meta, "id", folder.c_str());
        mod->name = JsonField(mod->ctx, meta, "name", mod->id.c_str());
        mod->main = JsonField(mod->ctx, meta, "main", "main.js");
        mod->ui   = JsonField(mod->ctx, meta, "ui", "");

        JSValue enabled = JS_GetPropertyStr(mod->ctx, meta, "enabled");
        const bool byDefault = JS_IsUndefined(enabled) || JS_ToBool(mod->ctx, enabled) != 0;
        JS_FreeValue(mod->ctx, enabled);
        JS_FreeValue(mod->ctx, meta);

        // mod.json says what a mod does by default; the Mods menu, what the
        // player chose.
        mod->enabled = ModSettings::Enabled(mod->id, byDefault);
        if (!mod->enabled)
        {
            LogJs("%s: switched off (%s)", mod->id.c_str(),
                  byDefault ? "Mods menu" : "mod.json");
            JS_FreeContext(mod->ctx);
            delete mod;
            return nullptr;
        }

        mod->handlers = JS_NewObject(mod->ctx);

        JSValue global = JS_GetGlobalObject(mod->ctx);
        JSValue speed = JS_NewObject(mod->ctx);
        RegisterCore(mod->ctx, speed, mod);
        RegisterMemory(mod->ctx, speed, mod);
        RegisterGame(mod->ctx, speed, mod);
        RegisterUi(mod->ctx, speed, mod);
        RegisterAudio(mod->ctx, speed, mod);
        RegisterEngine(mod->ctx, speed, mod);
        RegisterCommands(mod->ctx, speed, mod);
        RegisterStore(mod->ctx, speed, mod);
        RegisterDraw(mod->ctx, speed, mod);
        RegisterSettings(mod->ctx, speed, mod);
        JS_SetPropertyStr(mod->ctx, global, "speed", speed);
        JS_FreeValue(mod->ctx, global);

        std::string mainPath = mod->dir + "\\" + mod->main;
        std::string source;
        if (!ReadFile(mainPath, &source))
        {
            LogJs("%s: entry point not found (%s)", mod->id.c_str(), mainPath.c_str());
            JS_FreeValue(mod->ctx, mod->handlers);
            JS_FreeContext(mod->ctx);
            delete mod;
            return nullptr;
        }

        g_mods.push_back(mod);

        JSValue r = JS_Eval(mod->ctx, source.c_str(), source.size(),
                            mainPath.c_str(), JS_EVAL_TYPE_MODULE);
        if (JS_IsException(r)) ReportException(mod->ctx, "main");
        JS_FreeValue(mod->ctx, r);
        PumpJobs();

        LogJs("%s loaded (%s)%s", mod->id.c_str(), mod->name.c_str(),
              mod->ui.empty() ? "" : " + UI");
        return mod;
    }

    namespace
    {
        struct Posted { Mod* from; std::string event, json; };
        std::vector<Posted> g_posted;
    }

    void Post(Mod* from, const std::string& event, const std::string& json)
    {
        // Bounded: a mod that emits every frame with nobody listening must not
        // grow this without end.
        if (g_posted.size() < 1024) g_posted.push_back({ from, event, json });
    }

    void Frame()
    {
        // Taken out first: a handler may emit again, and that goes to the next
        // frame rather than into this loop.
        std::vector<Posted> posted;
        posted.swap(g_posted);
        for (const Posted& p : posted)
            for (size_t i = 0; i < g_mods.size(); i++)
                if (g_mods[i] != p.from) EmitTo(g_mods[i], p.event.c_str(), p.json);

        for (size_t i = 0; i < g_mods.size(); i++)
        {
            RunTimers(g_mods[i]);
            CallHandlers(g_mods[i], "frame", JS_UNDEFINED);
        }
        PumpJobs();
    }

    void EmitTo(Mod* mod, const char* event, const std::string& json)
    {
        JSValue arg = json.empty()
            ? JS_UNDEFINED
            : JS_ParseJSON(mod->ctx, json.c_str(), json.size(), "<event>");
        if (JS_IsException(arg)) { ReportException(mod->ctx, event); arg = JS_UNDEFINED; }

        CallHandlers(mod, event, arg);
        JS_FreeValue(mod->ctx, arg);
        PumpJobs();
    }

    void Emit(const char* event, const std::string& json)
    {
        for (size_t i = 0; i < g_mods.size(); i++)
            EmitTo(g_mods[i], event, json);
    }

    void DispatchUiMessage(const std::string& channel, const std::string& json)
    {
        // The channel arrives as "<modId>:<channel>": there is one UI, but many
        // mods inside it.
        size_t colon = channel.find(':');
        if (colon == std::string::npos)
        {
            LogJs("message from the UI with no mod: %s", channel.c_str());
            return;
        }

        std::string modId = channel.substr(0, colon);
        std::string name = channel.substr(colon + 1);

        // "sl" is the loader itself, not a mod: the command bar lives in the
        // shell and belongs to no one in particular.
        if (modId == "sl")
        {
            if (name == "command-close")
            {
                // The bar gives the keyboard back to the game as soon as it
                // closes: leaving the capture on would freeze the car.
                InputRouter::SetCapturing(false);
                return;
            }

            if (name == "command")
            {
                // The line arrives JSON-encoded; strip the quotes it came in.
                std::string line = json;
                if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
                    line = line.substr(1, line.size() - 2);

                // Unescape the little that a typed line can carry.
                std::string out;
                for (size_t i = 0; i < line.size(); i++)
                {
                    if (line[i] == 0x5C && i + 1 < line.size()) { out += line[++i]; }
                    else out += line[i];
                }
                RunCommand(out);
            }
            return;
        }

        for (size_t i = 0; i < g_mods.size(); i++)
        {
            if (g_mods[i]->id != modId) continue;

            Mod* mod = g_mods[i];
            JSValue arg = json.empty()
                ? JS_UNDEFINED
                : JS_ParseJSON(mod->ctx, json.c_str(), json.size(), "<ui>");
            if (JS_IsException(arg)) { ReportException(mod->ctx, "ui"); arg = JS_UNDEFINED; }

            JSValue payload = JS_NewObject(mod->ctx);
            JS_SetPropertyStr(mod->ctx, payload, "channel",
                              JS_NewString(mod->ctx, name.c_str()));
            JS_SetPropertyStr(mod->ctx, payload, "data", JS_DupValue(mod->ctx, arg));

            CallHandlers(mod, ("ui:" + name).c_str(), arg);
            CallHandlers(mod, "ui", payload);

            JS_FreeValue(mod->ctx, payload);
            JS_FreeValue(mod->ctx, arg);
            PumpJobs();
            return;
        }
    }
}
