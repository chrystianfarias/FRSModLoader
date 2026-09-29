#pragma once

#include <windows.h>

// Decides who gets keyboard and mouse: the game or the UI.
//
// NFSU2 does not read the keyboard through any window message (see the Pops
// notes), which works in our favour here: we can intercept WM_KEY* without
// taking anything away from the game. It does use the mouse through
// DirectInput, so while the UI has the mouse, the game's clicks are swallowed.
//
// The keyboard goes to the UI only on F1 (or when a mod asks). The mouse needs
// no key: where the page drew something, clicks and the wheel are the page's;
// where the game shows through, the game's.
namespace InputRouter
{
    void Install(int toggleVirtualKey);
    void Update();          // per frame: hooks the window as soon as it exists
    void Shutdown();

    bool IsCapturing();
    // Capturing with the mouse too (the default), not keyboard-only.
    bool IsCapturingMouse();
    // Whether the page has the mouse right now: captured, or - with no F1 -
    // the pointer over something the page drew, or a press that began there.
    // The game's clicks are held back while it does.
    bool MouseOnUi();

    // Whether "/" was typed since the last call (by character, whatever the
    // keyboard layout): the main loop opens the command bar then.
    bool TakeCommandRequest();
    // With `mouse` false only the keyboard goes to the UI: the mouse stays
    // with the game and no cursor appears. For screens driven the way the
    // game's own are, arrows, Enter and Esc.
    void SetCapturing(bool capturing, bool mouse = true);

    // Keys pressed while the UI is NOT capturing, for mods to listen to
    // (`speed.on("keydown", ...)`). Returns 0 when the queue drains.
    //
    // The queue exists because a WndProc is no place to run JavaScript: a slow
    // mod there would freeze the window. The main loop drains it once a frame.
    int PopKey();
}
