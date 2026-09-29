// speed.game - what the SDK already knows about NFSU2, ready to use.
//
// All of this could be written in JS with speed.mem, and that is the point:
// these are the addresses that already cost reverse-engineering effort (see
// NOTES.md), wrapped so a mod does not have to rediscover them.
#include "Api.h"

#include "game/CarFx.h"

#include "game/Game.h"
#include "game/Menu.h"
#include "game/Vehicle.h"

namespace
{
    JSValue Ptr(JSContext* ctx, void* p)
    {
        return p ? JS_NewInt64(ctx, (int64_t)(uintptr_t)p) : JS_NULL;
    }

    JSValue State(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        return JS_NewInt32(ctx, (int)Game::State());
    }

    JSValue StateName(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        switch (Game::State())
        {
        case BOOT_STATE:     return JS_NewString(ctx, "boot");
        case FRONTEND_STATE: return JS_NewString(ctx, "frontend");
        case 4: case 5:      return JS_NewString(ctx, "loading");
        case GAMEPLAY_STATE: return JS_NewString(ctx, "gameplay");
        default:             return JS_NewString(ctx, "unknown");
        }
    }

    JSValue IsRacing(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        return Game::State() == GAMEPLAY_STATE ? JS_TRUE : JS_FALSE;
    }

    JSValue HasFocus(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        return Game::HasFocus() ? JS_TRUE : JS_FALSE;
    }

    JSValue Player(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return Ptr(ctx, Game::Player()); }

    // The career car in use. Zero when there is none.
    // The car's record in the garage. It is where the career vehicle's
    // persistent data lives — installed parts included —, so exposing it is
    // what lets a mod investigate upgrades without another find in C++.
    JSValue CarEntryJs(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        void* e = Game::CarEntry();
        return e ? JS_NewInt64(ctx, (int64_t)(uintptr_t)e) : JS_NULL;
    }

    JSValue CarId(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        int id = Game::CarId();
        return id ? JS_NewInt32(ctx, id) : JS_NULL;
    }

    // The model name, through the game's own lookup.
    // The car's effect rules, as the game loaded them.
    JSValue FxRules(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        JSValue arr = JS_NewArray(ctx);
        int n = CarFx::Count();

        for (int i = 0; i < n; i++)
        {
            char* r = (char*)CarFx::Rule(i);
            if (!r) break;

            JSValue o = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, o, "tipo",   JS_NewInt32(ctx, *(int*)(r + FX_RULE_TYPE)));
            JS_SetPropertyStr(ctx, o, "indice", JS_NewInt32(ctx, *(int*)(r + FX_RULE_INDEX)));
            JS_SetPropertyStr(ctx, o, "efeito", JS_NewInt32(ctx, *(int*)(r + FX_RULE_EFFECT)));
            JS_SetPropertyStr(ctx, o, "valor",  JS_NewFloat64(ctx, *(float*)(r + FX_RULE_FLOAT)));
            JS_SetPropertyUint32(ctx, arr, (uint32_t)i, o);
        }
        return arr;
    }

    JSValue CarModel(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        const char* name = Game::CarModel();
        return name ? JS_NewString(ctx, name) : JS_NULL;
    }

    JSValue Car(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return Ptr(ctx, Game::Car()); }

    JSValue Gearbox(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return Ptr(ctx, Game::Gearbox()); }

    JSValue Physics(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return Ptr(ctx, Vehicle::Physics()); }

    JSValue EngineObj(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return Ptr(ctx, Vehicle::Engine()); }

    JSValue Window(JSContext* ctx, JSValueConst, int, JSValueConst*)
    { return Ptr(ctx, Game::Window()); }

    // { speed, rpm, throttle, distance, gear, boost } or null outside a race.
    //
    // Speed, RPM and throttle come from the car mirror, the live copy the game
    // uses for its dial. The gear only exists in the gearbox snapshot, written
    // on a shift - hence a second object. Boost comes from the engine object,
    // which only exists once the physics hook has caught the vehicle.
    JSValue Telemetry(JSContext* ctx, JSValueConst, int, JSValueConst*)
    {
        char* car = (char*)Game::Car();
        if (!car) return JS_NULL;

        float speedMs = *(float*)(car + CAR_SPEED);

        JSValue out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "speed",    JS_NewFloat64(ctx, speedMs));
        JS_SetPropertyStr(ctx, out, "kmh",      JS_NewFloat64(ctx, speedMs * 3.6));
        JS_SetPropertyStr(ctx, out, "mph",      JS_NewFloat64(ctx, speedMs * 2.2369363));
        JS_SetPropertyStr(ctx, out, "rpm",      JS_NewFloat64(ctx, *(float*)(car + CAR_RPM)));
        JS_SetPropertyStr(ctx, out, "throttle", JS_NewFloat64(ctx, *(float*)(car + CAR_THROTTLE)));
        JS_SetPropertyStr(ctx, out, "distance", JS_NewFloat64(ctx, *(float*)(car + CAR_DISTANCE)));

        DWORD* gearbox = Game::Gearbox();
        JS_SetPropertyStr(ctx, out, "gear",
                          gearbox ? JS_NewInt32(ctx, *(int*)((char*)gearbox + 0x34))
                                  : JS_NULL);

        char* engine = (char*)Vehicle::Engine();
        if (engine)
        {
            JS_SetPropertyStr(ctx, out, "boost",
                              JS_NewFloat64(ctx, *(float*)(engine + ENGINE_BOOST)));
            JS_SetPropertyStr(ctx, out, "boostMax",
                              JS_NewFloat64(ctx, *(float*)(engine + ENGINE_BOOST_MAX)));
            JS_SetPropertyStr(ctx, out, "boostMin",
                              JS_NewFloat64(ctx, *(float*)(engine + ENGINE_BOOST_MIN)));
            JS_SetPropertyStr(ctx, out, "idle",
                              JS_NewFloat64(ctx, *(float*)(engine + ENGINE_IDLE)));
            JS_SetPropertyStr(ctx, out, "redline",
                              JS_NewFloat64(ctx, *(float*)(engine + ENGINE_REDLINE)));
        }
        else
        {
            JS_SetPropertyStr(ctx, out, "boost",    JS_NULL);
            JS_SetPropertyStr(ctx, out, "boostMax", JS_NULL);
            JS_SetPropertyStr(ctx, out, "boostMin", JS_NULL);
            JS_SetPropertyStr(ctx, out, "idle",     JS_NULL);
            JS_SetPropertyStr(ctx, out, "redline",  JS_NULL);
        }
        return out;
    }

    JSValue SendFrontendMessage(JSContext* ctx, JSValueConst, int argc,
                                JSValueConst* argv)
    {
        if (argc < 2)
            return JS_ThrowTypeError(ctx,
                "speed.game.sendFrontendMessage(hash, packageName)");

        double hash = 0;
        JS_ToFloat64(ctx, &hash, argv[0]);
        const char* pkg = JS_ToCString(ctx, argv[1]);
        if (!pkg) return JS_EXCEPTION;

        FEngSendMessageToPackage((unsigned int)hash, pkg);
        JS_FreeCString(ctx, pkg);
        return JS_UNDEFINED;
    }

    JSValue HashUpper(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
    {
        if (argc < 1) return JS_ThrowTypeError(ctx, "speed.game.hash(text)");
        const char* s = JS_ToCString(ctx, argv[0]);
        if (!s) return JS_EXCEPTION;

        JSValue out = JS_NewInt64(ctx, (int64_t)(uint32_t)FEHashUpper(s));
        JS_FreeCString(ctx, s);
        return out;
    }
}

namespace Js
{
    void RegisterGame(JSContext* ctx, JSValue speed, Mod*)
    {
        JSValue game = JS_NewObject(ctx);

        JS_SetPropertyStr(ctx, game, "state",     JS_NewCFunction(ctx, State, "state", 0));
        JS_SetPropertyStr(ctx, game, "stateName", JS_NewCFunction(ctx, StateName, "stateName", 0));
        JS_SetPropertyStr(ctx, game, "isRacing",  JS_NewCFunction(ctx, IsRacing, "isRacing", 0));
        JS_SetPropertyStr(ctx, game, "hasFocus",  JS_NewCFunction(ctx, HasFocus, "hasFocus", 0));
        JS_SetPropertyStr(ctx, game, "player",    JS_NewCFunction(ctx, Player, "player", 0));
        JS_SetPropertyStr(ctx, game, "car",       JS_NewCFunction(ctx, Car, "car", 0));
        JS_SetPropertyStr(ctx, game, "carId",     JS_NewCFunction(ctx, CarId, "carId", 0));
        JS_SetPropertyStr(ctx, game, "carEntry", JS_NewCFunction(ctx, CarEntryJs, "carEntry", 0));
        JS_SetPropertyStr(ctx, game, "carModel",  JS_NewCFunction(ctx, CarModel, "carModel", 0));
        JS_SetPropertyStr(ctx, game, "fxRules",   JS_NewCFunction(ctx, FxRules, "fxRules", 0));
        JS_SetPropertyStr(ctx, game, "gearbox",   JS_NewCFunction(ctx, Gearbox, "gearbox", 0));
        JS_SetPropertyStr(ctx, game, "physics",   JS_NewCFunction(ctx, Physics, "physics", 0));
        JS_SetPropertyStr(ctx, game, "engine",    JS_NewCFunction(ctx, EngineObj, "engine", 0));
        JS_SetPropertyStr(ctx, game, "window",    JS_NewCFunction(ctx, Window, "window", 0));
        JS_SetPropertyStr(ctx, game, "telemetry", JS_NewCFunction(ctx, Telemetry, "telemetry", 0));
        JS_SetPropertyStr(ctx, game, "hash",      JS_NewCFunction(ctx, HashUpper, "hash", 1));
        JS_SetPropertyStr(ctx, game, "sendFrontendMessage",
                          JS_NewCFunction(ctx, SendFrontendMessage,
                                          "sendFrontendMessage", 2));

        // The raw addresses, for going past what the SDK wraps.
        JSValue addr = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, addr, "gameFlowManager", JS_NewInt64(ctx, _TheGameFlowManager));
        JS_SetPropertyStr(ctx, addr, "hWnd",            JS_NewInt64(ctx, _hWnd));
        JS_SetPropertyStr(ctx, addr, "isLostFocus",     JS_NewInt64(ctx, _IsLostFocus));
        JS_SetPropertyStr(ctx, addr, "windowedMode",    JS_NewInt64(ctx, _WindowedMode));
        JS_SetPropertyStr(ctx, addr, "skipMovies",      JS_NewInt64(ctx, _SkipMovies));
        JS_SetPropertyStr(ctx, addr, "feng",            JS_NewInt64(ctx, _cFEng_pInstance));
        JS_SetPropertyStr(ctx, addr, "playersByNumber", JS_NewInt64(ctx, _PlayersByNumber));
        JS_SetPropertyStr(ctx, game, "addr", addr);

        // Offsets inside the car mirror, for anyone wanting to read or watch
        // fields that telemetry() does not wrap.
        JSValue off = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, off, "speed",    JS_NewInt32(ctx, CAR_SPEED));
        JS_SetPropertyStr(ctx, off, "throttle", JS_NewInt32(ctx, CAR_THROTTLE));
        JS_SetPropertyStr(ctx, off, "rpm",      JS_NewInt32(ctx, CAR_RPM));
        JS_SetPropertyStr(ctx, off, "distance", JS_NewInt32(ctx, CAR_DISTANCE));
        JS_SetPropertyStr(ctx, game, "offset", off);

        JS_SetPropertyStr(ctx, speed, "game", game);

        // speed.menu lives next to speed.game because it is the same thing:
        // the game's frontend, reached through its own API.
        Menu::Register(ctx, speed);
    }
}
