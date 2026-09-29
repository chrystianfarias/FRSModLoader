#pragma once

#include <d3d9.h>

// The few 2D primitives the loader draws itself, on top of the game, inside
// EndScene: the UI's quad, the boot card, the cursor.
//
// Everything is premultiplied alpha, the way CEF hands its frames over, so one
// blend state serves all of it. Colours go in as plain 0xAARRGGBB and are
// premultiplied here; textures have to be uploaded premultiplied already.
//
// The caller saves and restores the game's state (a state block) around Begin
// and the draws: nothing here puts anything back.
namespace Draw2D
{
    void Begin(IDirect3DDevice9* device);

    // 0xAARRGGBB, with `opacity` folded into the alpha, premultiplied.
    DWORD Premul(DWORD argb, float opacity = 1.0f);

    // A textured rectangle, tinted by `argb` (white = as it is).
    void Image(IDirect3DDevice9* device, IDirect3DTexture9* texture,
               float x, float y, float w, float h, DWORD argb = 0xFFFFFFFF,
               float opacity = 1.0f);

    // A convex polygon of `n` points (x, y pairs in `xy`), one colour per
    // point, which Direct3D interpolates: gradients come free.
    void Polygon(IDirect3DDevice9* device, const float* xy, const DWORD* argb,
                 int n, float opacity = 1.0f);

    // A plain rectangle, flat or with a gradient from left to right.
    void Rect(IDirect3DDevice9* device, float x, float y, float w, float h,
              DWORD left, DWORD right, float opacity = 1.0f);

    // A texture for the premultiplied BGRA pixels in `bgra` (width * height *
    // 4 bytes). Managed pool: it survives a Reset on its own.
    IDirect3DTexture9* Texture(IDirect3DDevice9* device, const void* bgra,
                               int width, int height);
}
