// speed.audio - sounds fired from the game thread.
//
// A mod's own sounds, not the game's: load a WAV, play it with volume and
// pitch. The engine is native (XAudio2) rather than the Chromium layer because
// these sounds answer to physics - a pop belongs to the gear change that caused
// it, and the UI is a process away, behind JSON and a message queue.
//
// Paths are resolved against the mod's folder, so a mod ships its sounds next
// to its code and refers to them by name.
#include "Api.h"

#include "core/Audio.h"
#include "core/Log.h"

#include <string>

namespace
{
    // "pop.wav" -> "<mod dir>\pop.wav"; "C:\..." and "D:/..." are left alone.
    std::string Resolve(Js::Mod* mod, const char* path)
    {
        std::string p = path ? path : "";
        bool absolute = p.size() > 1 && (p[1] == ':' || p[0] == '\\' || p[0] == '/');
        if (absolute || !mod) return p;
        return mod->dir + "\\" + p;
    }

    JSValue Load(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.audio.load(path)");

        const char* path = JS_ToCString(ctx, argv[0]);
        if (!path) return JS_EXCEPTION;

        int id = Audio::Load(Resolve(Js::Owner(ctx), path));
        JS_FreeCString(ctx, path);

        // 0 means it did not load; null is friendlier to check in JS than 0,
        // which is falsy but still a number.
        return id ? JS_NewInt32(ctx, id) : JS_NULL;
    }

    JSValue Play(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.audio.play(id, options)");

        int32_t id = 0;
        if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;

        double volume = 1.0, pitch = 1.0, fadeIn = 0.0;
        bool loop = false;
        if (argc >= 2 && JS_IsObject(argv[1]))
        {
            JSValue l = JS_GetPropertyStr(ctx, argv[1], "loop");
            loop = JS_ToBool(ctx, l) > 0;
            JS_FreeValue(ctx, l);

            JSValue f = JS_GetPropertyStr(ctx, argv[1], "fadeIn");
            if (!JS_IsUndefined(f)) JS_ToFloat64(ctx, &fadeIn, f);
            JS_FreeValue(ctx, f);

            JSValue v = JS_GetPropertyStr(ctx, argv[1], "volume");
            if (!JS_IsUndefined(v)) JS_ToFloat64(ctx, &volume, v);
            JS_FreeValue(ctx, v);

            JSValue p = JS_GetPropertyStr(ctx, argv[1], "pitch");
            if (!JS_IsUndefined(p)) JS_ToFloat64(ctx, &pitch, p);
            JS_FreeValue(ctx, p);
        }

        return JS_NewBool(ctx, Audio::Play(id, (float)volume, (float)pitch, loop,
                                           fadeIn > 0 ? (DWORD)fadeIn : 0));
    }

    JSValue Stop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) { Audio::StopAll(); return JS_UNDEFINED; }

        int32_t id = 0;
        if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;

        // speed.audio.stop(id, { fade: 400 }): down to silence first.
        double fade = 0.0;
        if (argc >= 2 && JS_IsObject(argv[1]))
        {
            JSValue f = JS_GetPropertyStr(ctx, argv[1], "fade");
            if (!JS_IsUndefined(f)) JS_ToFloat64(ctx, &fade, f);
            JS_FreeValue(ctx, f);
        }
        if (fade > 0) Audio::FadeOut(id, (DWORD)fade);
        else Audio::Stop(id);
        return JS_UNDEFINED;
    }

    JSValue StopAll(JSContext*, JSValueConst, int, JSValueConst*)
    { Audio::StopAll(); return JS_UNDEFINED; }

    JSValue Unload(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.audio.unload(id)");

        int32_t id = 0;
        if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
        return JS_NewBool(ctx, Audio::Unload(id));
    }

    JSValue Volume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc >= 1)
        {
            double v = 1.0;
            if (JS_ToFloat64(ctx, &v, argv[0])) return JS_EXCEPTION;
            Audio::SetMasterVolume((float)v);
        }
        return JS_NewFloat64(ctx, Audio::MasterVolume());
    }

    JSValue Ready(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return JS_NewBool(ctx, Audio::Ready()); }
}

namespace Js
{
    void RegisterAudio(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue audio = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, audio, "load",    JS_NewCFunction(ctx, Load, "load", 1));
        JS_SetPropertyStr(ctx, audio, "play",    JS_NewCFunction(ctx, Play, "play", 2));
        JS_SetPropertyStr(ctx, audio, "stop",    JS_NewCFunction(ctx, Stop, "stop", 2));
        JS_SetPropertyStr(ctx, audio, "stopAll", JS_NewCFunction(ctx, StopAll, "stopAll", 0));
        JS_SetPropertyStr(ctx, audio, "unload",  JS_NewCFunction(ctx, Unload, "unload", 1));
        JS_SetPropertyStr(ctx, audio, "volume",  JS_NewCFunction(ctx, Volume, "volume", 1));
        JS_SetPropertyStr(ctx, audio, "ready",   JS_NewCFunction(ctx, Ready, "ready", 0));

        JS_SetPropertyStr(ctx, speed, "audio", audio);
    }
}
