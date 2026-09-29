#pragma once

// "Mods", the first entry of the game's Options menu, and the list it opens.
//
// It is the loader's own, so it lives in the .asi: no mod, no JavaScript on the
// game side. Four pieces of the engine are taken over for it, all found in the
// disassembly of SPEED2.EXE v1.2:
//
//   0x4B6230  the Options screen's Setup. It does nothing but build its items
//             (Audio first) with IconOption_Create + AddOption, so an item
//             added before the original runs is the first one.
//   0x4FF9D0  the language lookup: hash in edx, a binary search over the table
//             Languages\*.bin loaded at 0x8383D8, the text in eax. "Mods" is
//             not in any language file, so our hash answers it.
//   0x4B6080  the Options screen's messages. Choosing an entry makes the
//             screen leave for a page and come back rebuilt; "Mods" opens
//             over it instead, so its choice is taken here and never reaches
//             the screen, which would otherwise wait for a page that never
//             comes and ignore every entry after it.
//   0x4901D0  the texture lookup, GetTextureInfo(hash, ...). Our icon is no
//             texture pack's either: the lookup of our hash returns a copy of
//             the Audio icon's TextureInfo (and of the platform block it
//             points to, where the Direct3D texture lives) holding our hash
//             and our texture (docs/brand/mods-icon.png, embedded).
//
// Choosing the entry opens ui/mods.html, the loader's page with the list of
// installed mods - each one's picture, its on/off switch and the settings it
// declares (host/ModSettings.h) - through the same panel API a native plugin
// uses.
namespace ModsMenu
{
    void Install();   // DllMain: the three detours
    void Tick();      // main loop: mounts the panel, opens it when chosen
}
