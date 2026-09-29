#pragma once

#include <string>

// The door for native mods: an .asi written in C++ asks FRSModLoader for a
// panel of HTML and talks to it by message, without a line of JavaScript.
//
// The interface those mods include is sdk/frsmodloader.h; this is the side that
// implements it. A panel is mounted in the same shell, in the same shadow root
// isolation and on the same channels as a JavaScript mod's page, so the page
// itself cannot tell which kind of mod is behind it.
//
// Everything a plugin calls is queued and applied on the game thread, in Tick:
// the caller may be on any thread, but CEF and the callbacks are not.
namespace Host
{
    void Install();
    void Tick();       // once per frame, on the game thread
    void Shutdown();

    // A message from the UI, before it reaches the JavaScript mods. Returns
    // true when a panel (or the shell talking to us) owned it.
    bool HandleUiMessage(const std::string& channel, const std::string& json);
}
