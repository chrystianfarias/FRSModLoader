#pragma once

#include <windows.h>
#include <stdint.h>

#include "core/Hook.h"
#include "core/Log.h"

// The car's effect rules (CARFX).
//
// The engine has 46 effects, named in a table at 0x8027C0 and numbered by
// 0x430C90 — 10 NITRO, 11 EXHAUST_SMOKE, 12 EXHAUST_BLOWOFF, 13 NOS_BLOWOFF.
// Which of them a car actually uses, and under what condition, comes from
// DATA: rules loaded per car.
//
// The loader is at 0x432CA4, and the shape of what it builds is right there in
// the call:
//
//     mov ecx, [esi+0x14]     ; how many rules
//     push 0x430DB0           ; the per-rule constructor
//     push 0x1C               ; 28 bytes each
//     push ecx
//     lea edx, [esi+0x18]     ; the array
//     push edx
//     call 0x5D40F0
//
// A rule holds its type at +0x00, an index (< 8) at +0x08, the EFFECT ID at
// +0x0C and a float at +0x14.
//
// Finding the container by scanning memory does not work: ids 10 to 13 are
// small integers that appear thousands of times, and a two-field pattern lets
// hundreds of false positives through. Catching `esi` as the loader runs is
// exact, and costs one hook.
//
// Prologue: mov (3) + push imm32 (5) = 8 bytes, neither relative.
namespace CarFx
{
    #define FX_LOADER       0x432CA4
    #define FX_LOADER_LEN   8
    #define FX_COUNT        0x14
    #define FX_ARRAY        0x18
    #define FX_RULE_SIZE    0x1C
    #define FX_RULE_TYPE    0x00
    #define FX_RULE_INDEX   0x08
    #define FX_RULE_EFFECT  0x0C
    #define FX_RULE_FLOAT   0x14

    inline void*     g_container = 0;
    inline uintptr_t g_continue = 0;

    inline void __cdecl Capture(void* self)
    {
        if (!self) return;
        g_container = self;

        // Logs the first captures with the raw count: if the loader runs but
        // the count comes out absurd, the `esi` here is not the container I
        // assumed — and without this log both cases are indistinguishable
        // from "nothing loaded".
        static int seen = 0;
        if (seen < 5)
        {
            seen++;
            int n = IsBadReadPtr((char*)self + FX_COUNT, 4)
                  ? -1 : *(int*)((char*)self + FX_COUNT);
            Log("carfx: loader at 0x%p, raw count = %d", self, n);
        }
    }

    __declspec(naked) inline void LoaderHook()
    {
        __asm {
            pushad
            pushfd
            push esi            // the container being filled
            call Capture
            add  esp, 4
            popfd
            popad
            jmp dword ptr [g_continue]
        }
    }

    inline bool Install()
    {
        static Trampoline tramp;
        g_continue = (uintptr_t)tramp.Install(FX_LOADER, (void*)LoaderHook,
                                              FX_LOADER_LEN);
        if (!g_continue)
        {
            Log("carfx: could not hook the rule loader at 0x%08X", FX_LOADER);
            return false;
        }

        Log("carfx: rule loader hooked at 0x%08X", FX_LOADER);
        return true;
    }

    inline void* Container() { return g_container; }

    inline int Count()
    {
        if (!g_container || IsBadReadPtr((char*)g_container + FX_COUNT, 4)) return 0;
        int n = *(int*)((char*)g_container + FX_COUNT);
        return (n > 0 && n < 4096) ? n : 0;
    }

    inline void* Rule(int i)
    {
        if (i < 0 || i >= Count()) return 0;
        char* r = (char*)g_container + FX_ARRAY + i * FX_RULE_SIZE;
        return IsBadReadPtr(r, FX_RULE_SIZE) ? 0 : r;
    }
}
