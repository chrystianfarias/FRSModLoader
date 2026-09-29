#pragma once

#include <windows.h>
#include <stdint.h>

#include "core/Hook.h"
#include "core/Log.h"
#include "game/Game.h"
#include "game/Vehicle.h"

// Writing back into the engine: a rev limiter on a button, and anti-lag.
//
// This is native and not JavaScript for one reason, and it is worth stating
// plainly because it cost three failed attempts to learn. The order inside a
// frame is
//
//     input writes engine+0x80  ->  physics consumes  ->  0x5A5540  ->  mods
//
// so anything a "frame" callback writes lands after the physics already read
// it. What the mod decides (limit, hysteresis, which key) stays in JS; what has
// to happen at an exact point in the frame lives here.
//
// Two interventions, both riding the game's own machinery rather than fighting
// it:
//
//   cut       the game already cuts ignition on every gear change. At 0x5A1629
//             it checks physics+0x5C — a TIMESTAMP of when the cut started —
//             and zeroes the throttle while it is non-zero, clearing it after
//             0.45s. Stamping it holds the cut; clearing it ends the cut early,
//             which is what a launch limiter needs (0.45s is tuned for a shift,
//             where dropping the revs is the point).
//
//   anti-lag  the turbo model (0x5A0F30, called from one place) is an
//             integrator. Writing the pressure from outside only produces a
//             sawtooth. Instead the call is wrapped and the function runs
//             seeing a full throttle, so the game's own model fills the turbo
//             with its own curve — which is what anti-lag does in the metal:
//             the exhaust keeps the turbine spinning while the ignition is cut.
namespace EngineControl
{
    #define CUT_CHECK_SITE   0x5A1629   // fld [esi+0x5C]; fcomp [0x7D7D3C]
    #define CUT_CHECK_LEN    9          // both instructions, neither relative
    #define PHYSICS_CUT      0x5C
    #define GAME_CLOCK_ADDR  0x86518C
    #define CLOCK_SCALE      0.00025f
    #define RPM_TO_RAD       0.1047197551f

    // Set from JS; read inside the frame.
    inline bool  g_enabled     = false;
    inline float g_limitRpm    = 4500.0f;
    inline float g_hysteresis  = 150.0f;
    inline bool  g_engaged     = false;   // the button is held
    inline bool  g_antiLag     = true;
    inline bool  g_holdOnShift = true;

    // Read back by JS, for the panel.
    inline bool  g_cutting  = false;
    inline int   g_cutCount = 0;

    inline void* g_drivetrain = 0;

    // Set by the mod when it sees a gear change, so anti-lag can also carry the
    // boost across a shift. The shift does NOT go through the cut timer above -
    // that is a different mechanism - so it cannot be detected from here.
    inline DWORD g_shiftUntil = 0;

    inline float Now()
    {
        return (float)(*(int*)GAME_CLOCK_ADDR) * CLOCK_SCALE;
    }

    inline float Rpm()
    {
        void* engine = Vehicle::Engine();
        if (!engine) return -1.0f;
        return *(float*)((char*)engine + ENGINE_RPM) / RPM_TO_RAD;
    }

    // ------------------------------------------------------------ the cut
    //
    // Runs at the exact point the game decides whether to cut, with `this` in
    // esi and the RPM of THIS frame - deciding a frame earlier, from the main
    // loop, arrives late enough to matter.
    inline void __cdecl OnCutCheck(void* self)
    {
        g_drivetrain = self;

        if (!self || IsBadWritePtr((char*)self + PHYSICS_CUT, 4)) return;
        float* timer = (float*)((char*)self + PHYSICS_CUT);

        if (!g_enabled || !g_engaged)
        {
            // Releasing has to CLEAR the stamp, not merely stop renewing it:
            // a stamp left fresh keeps the game cutting for the rest of its
            // 0.45s, and the engine falls away exactly when the launch needs it.
            if (g_cutting) { g_cutting = false; *timer = 0.0f; }
            return;
        }

        float rpm = Rpm();
        if (rpm < 0.0f) return;

        if (rpm >= g_limitRpm)
        {
            if (!g_cutting) { g_cutting = true; g_cutCount++; }
            *timer = Now();
        }
        else if (rpm <= g_limitRpm - g_hysteresis)
        {
            g_cutting = false;
            *timer = 0.0f;
        }
    }

    inline uintptr_t g_cutContinue = 0;

    __declspec(naked) inline void CutThunk()
    {
        __asm {
            pushad
            pushfd
            push esi                 // the drivetrain object of this call
            call OnCutCheck
            add  esp, 4
            popfd
            popad
            jmp  dword ptr [g_cutContinue]
        }
    }

    // ------------------------------------------------------- the turbo wrap
    inline void*  g_turboOriginal   = 0;
    inline void*  g_turboEngine     = 0;
    inline DWORD  g_savedThrottle   = 0;
    inline bool   g_restoreThrottle = false;
    inline float  g_lastRealThrottle = 0.0f;

    // Both halves write the throttle as a DWORD rather than a float on purpose:
    // the wrapped function returns a value on the x87 stack, and touching the
    // FPU here would unbalance it.
    inline void __cdecl BeforeTurbo(void* engine)
    {
        g_restoreThrottle = false;
        if (!g_antiLag || !engine) return;
        if (IsBadWritePtr((char*)engine + ENGINE_THROTTLE, 4)) return;

        float real = *(float*)((char*)engine + ENGINE_THROTTLE);
        bool cutting = g_cutting;
        if (!cutting) g_lastRealThrottle = real;

        // The limiter's own cut, or a gear change under load. A shift with the
        // foot off has no pressure worth keeping.
        bool shifting = g_holdOnShift && GetTickCount() < g_shiftUntil
                     && g_lastRealThrottle > 0.5f;

        // And the launch, which needs a third case of its own.
        //
        // With the button held, the shift from N to 1 opened a window with
        // nobody holding the pressure: the revs drop, our cut goes off, and
        // the mod's shift detection depends on reading the throttle — which
        // reads low during the cut, so it never even happens. The pressure
        // leaked out right there, and the launch came out weak.
        //
        // While the button is pressed the driver is explicitly spooling the
        // turbo, and whichever gear the gearbox is in does not change that. It
        // depends neither on the cut nor on a detected shift, which was the
        // weak spot.
        bool launching = g_engaged;

        if (!cutting && !shifting && !launching) return;

        g_turboEngine = engine;
        DWORD* gas = (DWORD*)((char*)engine + ENGINE_THROTTLE);
        g_savedThrottle = *gas;
        *gas = 0x3F800000;             // 1.0f
        g_restoreThrottle = true;
    }

    inline void __cdecl AfterTurbo()
    {
        if (!g_restoreThrottle) return;
        g_restoreThrottle = false;
        *(DWORD*)((char*)g_turboEngine + ENGINE_THROTTLE) = g_savedThrottle;
    }

    // __thiscall with one stack argument, and the original cleans it with
    // `ret 4`. The argument therefore has to be pushed again for the original -
    // calling it directly leaves the argument one slot too deep, and its `ret 4`
    // then eats the CALLER's return address. That mistake crashed the game.
    __declspec(naked) inline void TurboWrapper()
    {
        __asm {
            pushad
            push ecx                        // this = the engine
            call BeforeTurbo
            add  esp, 4
            popad

            push dword ptr [esp + 4]        // a copy of the argument
            call dword ptr [g_turboOriginal]

            pushad
            call AfterTurbo
            popad
            ret  4                          // and ours is cleaned here
        }
    }

    // ---------------------------------------------------------------- setup
    inline bool Install()
    {
        static Trampoline tramp;
        g_cutContinue = (uintptr_t)tramp.Install(CUT_CHECK_SITE,
                                                 (void*)CutThunk, CUT_CHECK_LEN);
        if (!g_cutContinue)
        {
            Log("engine: could not hook the ignition cut at 0x%08X", CUT_CHECK_SITE);
            return false;
        }

        g_turboOriginal = (void*)RedirectCall(TURBO_CALL_SITE, (void*)TurboWrapper);

        Log("engine: ignition cut at 0x%08X, turbo wrapped at 0x%08X",
            CUT_CHECK_SITE, TURBO_CALL_SITE);
        return true;
    }
}
