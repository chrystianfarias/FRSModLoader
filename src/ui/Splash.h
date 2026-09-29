#pragma once

#include <d3d9.h>

// The loader's card on the boot splash ("press a button"): wordmark, how many
// mods loaded, the version. It is the loader's, so it lives in the .asi and is
// drawn with Direct3D - no mod, no page, no JavaScript. It shows even with
// Chromium off ([UI] Enabled = 0), and [UI] SplashCard = 0 turns it off.
//
// Knowing we are on the splash took three tries, and the log settled each one:
//
//   1. FEngFindPackage("Chyron.fng") - never resolved, even with the splash
//      sitting there for thirty seconds. The mechanism is fine; the name is
//      what is wrong.
//   2. The gameflow state - `3 (frontend)` from the first second, splash on
//      screen. The splash lives inside the frontend state, same as the menus,
//      so the state cannot tell them apart.
//   3. What did move: the byte at 0x836495 goes 0 -> 1 around 4.9s, which is
//      the splash becoming ready (Pops found it as the latch the Chyron screen
//      checks before accepting its advance event). It never goes back to 0,
//      so it marks "the splash has appeared", not "the splash is up".
//
// So: a list of names for the splash package is tried, and while none of them
// is the one, the shape of the boot decides - the splash is what stands between
// the latch flipping and the main menu coming alive.
namespace Splash
{
    void Init();          // reads the ini
    void Tick();          // once per frame, from the main loop
    void OnKey();         // a key press the game saw: "press a button"

    bool Visible();
    // Inside EndScene. `width` x `height` is the backbuffer.
    void Draw(IDirect3DDevice9* device, int width, int height);
}
