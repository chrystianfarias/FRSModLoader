#pragma once

#include <stdint.h>
#include <string>
#include <vector>

// The game's own interface art, served to the page at runtime.
//
// Underground 2's menus are not drawn, they are textured: the pill button, its
// glow, the arrow rings, the icons on the Pause screen all live in the game's
// texture packs (FRONTEND\FRONTB.LZC, GLOBAL\GLOBALB.BUN, ...). This reads
// those packs from the game folder - the player's own copy, never a file of
// ours - and hands any texture in them to Chromium as a PNG:
//
//     http://nfsu2.tex/UI_PC_GENERIC_BUTTON
//     http://nfsu2.tex/UI_PC_GENERIC_BUTTON_GLOW?tint=b9e75b
//     http://nfsu2.tex/U2_MENU_ARROW?flip=h
//     http://nfsu2.tex/U2_MENU_ROUNDEDCORNER?mirror  (a quarter made whole)
//     http://nfsu2.tex/U2_MENU_BOX?stack=3           (three layers of it)
//     http://nfsu2.tex/0x26A17126              (by hash)
//     http://nfsu2.tex/TRACKMAP4000            (a minimap, TRACKS\TRACKMAP<id>.BIN)
//     http://nfsu2.tex/mod/frsminimap/maps/TRACKMAP4000.dds
//                                              (a .dds a mod ships in its own folder -
//                                               loose, or inside a zip in the mod's
//                                               root - decoded the same way)
//     http://nfsu2.tex/index.json              (everything there is)
//
// Nothing is extracted to disk and nothing ships with the loader, so a
// texture mod that replaces the game's packs is what the page shows as well.
//
// No CEF in here: CefHost owns the scheme handler and calls Get.
namespace GameTextures
{
    // `path` is the URL's path and query ("/NAME.png?tint=ff8800"). On success
    // fills `body` and `mime` and returns true; false means 404. Safe to call
    // from any thread: the packs are read on the first call, under a lock.
    bool Get(const std::string& path, std::string* body, std::string* mime);

    // The same texture as plain pixels, for whoever draws it with Direct3D
    // instead of handing it to a page: RGBA, straight alpha, row by row.
    // Reading the packs the first time takes a moment, so the first call
    // belongs on a thread of its own, not in EndScene.
    bool Rgba(const char* name, std::vector<uint8_t>* rgba, int* width, int* height);
}
