#pragma once

#include <d3d9.h>

// The game's own mouse pointer, drawn over the UI.
//
// The game draws its cursor (PC_CURSOR, in GLOBAL\GLOBALB.BUN) at the end of
// the frontend's render, 0x5378C0 -> 0x50B460, which is to say under the UI's
// quad: a page on top covers it. So the call is taken over. The game still
// decides WHEN there is a cursor - always in the frontend, elsewhere for two
// seconds after the mouse last moved - and the loader draws it, last, above
// everything. Windows' arrow stays hidden (InputRouter).
//
// What 0x50B460 does, from the disassembly:
//
//   moved = (x, y) != last (0x838498, 0x83849C)
//   if moved: last move = now (0x83A9CC, the game's clock at 0x8651AC, 4000
//             ticks a second); 0x8384A0 = moved
//   if (now - last move) / 4000 > 2.0 s and not in the frontend: no cursor
//   else: PC_CURSOR at its own size (32x32 on the 640x480 frontend), tip at
//         (12, 6) of the texture
//
// (x, y) come from 0x8763D4/0x8763D8, which 0x5BF750 fills from GetCursorPos
// scaled to 640x480 - the same pointer Windows has, so it is drawn where the
// real mouse is.
namespace Cursor
{
    // Takes over the game's cursor draw. From DllMain, with the other hooks.
    void Install();

    // Reads the texture out of the game's packs on a thread of its own: the
    // first read inflates a pack, and that has no place in a frame. Until it
    // is there, the game keeps drawing its own.
    void Preload();

    bool Visible();
    // Inside EndScene, after the UI. `width` x `height` is the backbuffer.
    void Draw(IDirect3DDevice9* device, int width, int height);
}
