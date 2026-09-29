#pragma once

#include <windows.h>
#include <stdint.h>

#include "core/Hook.h"
#include "core/Log.h"
#include "game/Game.h"

// Captures the vehicle PHYSICS object.
//
// The car mirror (player+0x04) gives speed, RPM and throttle, but stops at what
// the dial needs. Boost, wheels and the real engine live in physics, and
// physics is not hanging off any global: the way to get it is to wait for the
// game to call 0x5A5540, which is __thiscall and brings the object in ecx.
//
//   physics + 0x20 / + 0x48   engine object (two aliases for the same thing)
//   physics + 0x58            the car mirror
//   physics + 0x5C            timestamp of the game's own ignition cut
//   engine  + 0x14            engine speed, in RAD/S
//   engine  + 0x80            the input throttle, before the cut zeroes it
//   engine  + 0x9C            current boost pressure (negative under vacuum)
//   engine  + 0xA4            the instantaneous TARGET from the engine map -
//                             not the car's ceiling: it drops while the
//                             throttle is cut
//   engine  + 0xAC            vacuum floor
//
// One thing that is not obvious and costs a day to learn the hard way: the
// order inside a frame is
//
//     input writes engine+0x80  ->  physics consumes  ->  0x5A5540  ->  mods
//
// so anything written from a frame callback lands AFTER the physics already
// read it. Values that are inputs (throttle, the cut timestamp) have to be
// written from inside the pass; values that are state (RPM, boost) must not be
// written at all - pinning them does not cut power, it breaks the integrator
// that computes everything else from them, and the car accelerates without
// limit.
namespace Vehicle
{
    #define VEHICLE_UPDATE   0x5A5540
    #define PHYSICS_ENGINE   0x48
    // physics+0x58 points INTO the mirror, 0x40 past its start - not at the
    // mirror itself. Proof: the smoothed RPM is written at [physics+0x58]+0x400
    // and read from mirror+0x440, and a hardware breakpoint on mirror+0x470
    // fired on a write to [edx+0x430] with edx = [physics+0x58].
    //
    // Comparing the two directly never matches, so the capture below silently
    // never fired and boost was always null.
    #define PHYSICS_MIRROR   0x58
    #define MIRROR_SKEW      0x40

    // The ignition cut the game already has, and uses on every gear change.
    // physics+0x5C is a TIMESTAMP of when the cut began, not a countdown: the
    // check at 0x5A1629 cuts while it differs from zero and clears it once
    // 0.45s have passed. Stamp it with GAME_CLOCK * CUT_TIME_SCALE to cut;
    // write zero to end the cut early.
    #define PHYSICS_CUT_TIMER 0x5C
    #define CUT_ZERO          0x7D7D3C   // the 0.0 it is compared against
    #define CUT_DURATION      0x7A18C8   // 0.45 seconds
    #define CUT_TIME_SCALE    0x784268   // 0.00025
    #define GAME_CLOCK        0x86518C   // int counter; * scale = seconds

    // The engine. RPM here is in RAD/S, not rpm: divide by 2*pi/60.
    #define ENGINE_RPM       0x14
    #define ENGINE_IDLE      0x28   // 800 or 850, per car
    #define ENGINE_REDLINE   0x2C   // 7000 or 7500, per car
    #define ENGINE_MULT      0x78   // multiplier applied to the throttle
    #define ENGINE_THROTTLE  0x80   // the INPUT throttle (0 / 0.7 / 1.0)
    #define ENGINE_BOOST     0x9C
    #define ENGINE_BOOST_MAX 0xA4
    #define ENGINE_BOOST_MIN 0xAC

    // The turbo model: one function, one call site. Its integrator lives at
    // 0x5A1094 (pressure += increment), which is why writing the pressure from
    // outside only ever produces a sawtooth. To hold boost, wrap the call and
    // let the function run seeing a full throttle.
    #define TURBO_UPDATE     0x5A0F30
    #define TURBO_CALL_SITE  0x5AA7F0

    // Car effects (CARFX). The player's effects object hangs off the mirror.
    #define MIRROR_CARFX     0x554
    #define CARFX_NITRO           10
    #define CARFX_EXHAUST_SMOKE   11
    #define CARFX_EXHAUST_BLOWOFF 12
    #define CARFX_NOS_BLOWOFF     13

    // ---------------------------------------------------------- Geometry
    //
    // Everything below was measured inside the game, with a cube drawn in the
    // scene, and cost enough to be worth writing down.
    //
    // The WORLD is Z UP: a parked car has x,y in the hundreds and z in the
    // tens (the terrain height).
    //
    // The car's POSE (physics+0x20) holds the position at +0x20 and the
    // rotation at +0x30, three rows of four floats. The ROWS are the car's axes
    // in the world, so a local point becomes world by multiplying by the
    // COLUMNS:
    //
    //     world.x = pos.x + lx*m[0] + ly*m[4] + lz*m[8]
    //
    // The transpose also preserves distances, so NO numeric measurement tells
    // the two apart; what does is drawing both on screen — with the wrong one
    // the point is mirrored and only looks right from some angles.
    //
    // The pose position is the PHYSICS one (centre of mass), which is NOT the
    // model origin — and the model's markers are measured from the latter. The
    // difference is at pose+0x1F0, as (forward, side, height) in the car's
    // axes: it gives 0.300 on one car and 0.220 on another, and the side
    // component is always zero, because the centre of mass sits on the plane
    // of symmetry.
    #define POSE             0x20
    #define POSE_POSITION    0x20
    #define POSE_ROTATION    0x30
    #define POSE_MODEL_ORIGIN 0x1F0

    // The model's markers (LEFT_EXHAUST and friends) sit in a list at
    // carfx+0x598, built by the game — it already reflects the EQUIPPED
    // bumper, which is what spares us from mapping exhaust positions per kit.
    //
    // The list is circular and each node is 20 bytes:
    //
    //     +0x00  next (wraps back to the head on the last one)
    //     +0x04  x, y, z   in MODEL coordinates
    //     +0x10  pointer to the marker's orientation
    //
    // The orientation is a 3x4 matrix starting at +0x10 of the target: for the
    // exhaust the rows are (0,0,1), (0,1,0), (-1,0,0) — the marker's Z axis
    // points to the rear, which is where the pipe blows.
    #define CARFX_MARKERS    0x598
    #define MARKER_NEXT      0x00
    #define MARKER_POSITION  0x04
    #define MARKER_ORIENT    0x10
    #define MARKER_STRIDE    0x14

    inline void*     g_physics = 0;
    inline uintptr_t g_continue = 0;   // trampoline, or the hook already there

    // 0x5A5540 runs for EVERY vehicle in the world - traffic and opponents
    // included. Storing ecx from any call would make the object flip between
    // cars several times per frame, and boost in the HUD turned into noise.
    //
    // The filter is the mirror: only the vehicle whose physics+0x58 matches the
    // player's mirror is ours.
    inline void __cdecl Capture(void* self)
    {
        if (!self || IsBadReadPtr(self, PHYSICS_MIRROR + 4)) return;

        void* mirror = *(void**)((char*)self + PHYSICS_MIRROR);
        void* ours = (char*)Game::Car() + MIRROR_SKEW;
        if (mirror && mirror == ours) g_physics = self;
    }

    // Naked because this detour stands in for the function's prologue: it must
    // hand back every register and flag exactly as it found them.
    __declspec(naked) inline void VehicleHook()
    {
        __asm {
            pushad
            pushfd
            push ecx            // this = the vehicle of this call
            call Capture
            add  esp, 4
            popfd
            popad
            jmp dword ptr [g_continue]
        }
    }

    inline bool Install()
    {
        uint8_t* p = (uint8_t*)VEHICLE_UPDATE;

        if (*p == 0xE9)
        {
            // Someone got here first (Pops hooks this too). Their JMP cannot be
            // copied into a trampoline - a copied rel32 points at the wrong
            // place - so we install in front and jump to them: both run, in
            // installation order.
            g_continue = (uintptr_t)p + 5 + *(int32_t*)(p + 1);
            Log("physics: chaining to the previous hook (0x%08X)", g_continue);

            DWORD old;
            VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
            *(int32_t*)(p + 1) =
                (int32_t)((uintptr_t)VehicleHook - ((uintptr_t)p + 5));
            VirtualProtect(p, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p, 5);
        }
        else
        {
            size_t len = CopyLength(p, 5);
            if (len == 0)
            {
                Log("physics: prologue at 0x%08X I cannot copy"
                    " (%02X %02X %02X %02X %02X) - boost unavailable",
                    VEHICLE_UPDATE, p[0], p[1], p[2], p[3], p[4]);
                return false;
            }

            // Install already writes the JMP and returns the prologue trampoline.
            static Trampoline tramp;
            g_continue = (uintptr_t)tramp.Install(VEHICLE_UPDATE,
                                                  (void*)VehicleHook, len);
        }

        Log("physics: hooked at 0x%08X", VEHICLE_UPDATE);
        return true;
    }

    inline void* Physics()
    {
        if (!g_physics || IsBadReadPtr(g_physics, PHYSICS_MIRROR + 4)) return 0;

        // Check again on read: when the player changes car (or leaves the race)
        // the stored pointer goes stale, and reading from a freed object is
        // worse than showing no boost at all.
        if (*(void**)((char*)g_physics + PHYSICS_MIRROR)
            != (void*)((char*)Game::Car() + MIRROR_SKEW))
            return 0;

        return g_physics;
    }

    inline void* Engine()
    {
        void* phys = Physics();
        if (!phys) return 0;
        void* engine = *(void**)((char*)phys + PHYSICS_ENGINE);
        if (!engine || IsBadReadPtr(engine, 0xB0)) return 0;
        return engine;
    }
}
