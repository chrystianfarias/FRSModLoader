#include "Cursor.h"

#include "Draw2D.h"
#include "GameTextures.h"
#include "InputRouter.h"
#include "core/Config.h"
#include "core/Hook.h"
#include "core/Log.h"
#include "game/Game.h"

#include <windows.h>
#include <math.h>
#include <vector>

namespace
{
    // The call to the cursor's draw inside the frontend render (0x5378C0),
    // and what 0x50B460 reads and writes. See Cursor.h.
    const uintptr_t CURSOR_CALL_SITE = 0x537913;
    const uintptr_t GAME_CLOCK       = 0x8651AC;   // 4000 ticks a second
    const uintptr_t LAST_X           = 0x838498;
    const uintptr_t LAST_Y           = 0x83849C;
    const uintptr_t MOVED            = 0x8384A0;
    const uintptr_t LAST_MOVE        = 0x83A9CC;
    const float     TICKS_TO_SECONDS = 0.00025f;   // the game's 0x784268
    const float     HIDE_AFTER       = 2.0f;       // the game's 0x7844C0
    const float     HOT_X = 12.0f, HOT_Y = 6.0f;   // the tip, in texture pixels

    typedef void (__cdecl* tDrawCursor)(int x, int y);
    tDrawCursor g_gameDraw = 0;

    // Written once by the loading thread, then only read: `g_ready` goes up
    // after the pixels are in place, and nobody touches them before.
    std::vector<uint8_t> g_bgra;
    int                  g_w = 0, g_h = 0;
    volatile LONG        g_ready = 0;

    IDirect3DTexture9*   g_texture = 0;
    IDirect3DDevice9*    g_device = 0;   // the one g_texture belongs to
    bool                 g_failed = false;
    DWORD                g_wantedAt = 0; // when the game last asked for a cursor

    bool CanDraw() { return g_ready && !g_failed; }

    // Stands in for 0x50B460: the same bookkeeping, and the same decision,
    // but the drawing is left for EndScene, above the UI.
    void __cdecl DrawCursorHook(int x, int y)
    {
        // Nothing of ours to draw yet: the game's own is better than none.
        if (!CanDraw())
        {
            g_gameDraw(x, y);
            return;
        }

        const int now = *(int*)GAME_CLOCK;
        const bool moved = x != *(int*)LAST_X || y != *(int*)LAST_Y;
        *(BYTE*)MOVED = moved ? 1 : 0;
        if (moved) *(int*)LAST_MOVE = now;
        *(int*)LAST_X = x;
        *(int*)LAST_Y = y;

        const float idle = (now - *(int*)LAST_MOVE) * TICKS_TO_SECONDS;
        if (idle <= HIDE_AFTER || Game::State() == FRONTEND_STATE)
            g_wantedAt = GetTickCount();
    }

    DWORD WINAPI LoadThread(LPVOID)
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!GameTextures::Rgba("PC_CURSOR", &rgba, &w, &h) || w <= 0 || h <= 0)
        {
            LogGfx("cursor: PC_CURSOR not found in the game's packs");
            return 0;
        }

        // RGBA straight -> BGRA premultiplied, which is what Draw2D blends.
        g_bgra.resize(rgba.size());
        for (size_t i = 0; i < rgba.size(); i += 4)
        {
            unsigned a = rgba[i + 3];
            g_bgra[i + 0] = (uint8_t)(rgba[i + 2] * a / 255);
            g_bgra[i + 1] = (uint8_t)(rgba[i + 1] * a / 255);
            g_bgra[i + 2] = (uint8_t)(rgba[i + 0] * a / 255);
            g_bgra[i + 3] = (uint8_t)a;
        }
        g_w = w;
        g_h = h;
        InterlockedExchange(&g_ready, 1);
        LogGfx("cursor: PC_CURSOR %dx%d ready", w, h);
        return 0;
    }

    // Where the pointer is, in backbuffer pixels. False when it is outside
    // the window, where the cursor is not ours to draw.
    bool Position(int width, int height, float* x, float* y)
    {
        HWND hwnd = Game::Window();
        POINT p;
        RECT rc;
        if (!hwnd || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p) ||
            !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
            return false;
        if (p.x < 0 || p.y < 0 || p.x >= rc.right || p.y >= rc.bottom) return false;

        *x = (float)p.x * width / rc.right;
        *y = (float)p.y * height / rc.bottom;
        return true;
    }
}

namespace Cursor
{
    void Install()
    {
        g_gameDraw = (tDrawCursor)RedirectCall(CURSOR_CALL_SITE, (void*)DrawCursorHook);
        LogGfx("cursor: the game's draw at 0x%08X is ours (was 0x%08X)",
               CURSOR_CALL_SITE, (uintptr_t)g_gameDraw);
    }

    void Preload()
    {
        HANDLE t = CreateThread(NULL, 0, LoadThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }

    bool Visible()
    {
        if (!CanDraw()) return false;
        // The UI holding the mouse wants a pointer whatever the game thinks.
        return InputRouter::IsCapturingMouse() || GetTickCount() - g_wantedAt < 100;
    }

    void Draw(IDirect3DDevice9* device, int width, int height)
    {
        if (!Visible()) return;

        if (g_texture && g_device != device)
        {
            g_texture->Release();
            g_texture = 0;
        }
        if (!g_texture)
        {
            g_texture = Draw2D::Texture(device, g_bgra.data(), g_w, g_h);
            g_device = device;
            if (!g_texture)
            {
                g_failed = true;
                LogGfx("cursor: could not create its texture");
                return;
            }
        }

        float x, y;
        if (!Position(width, height, &x, &y)) return;

        // The frontend is laid out on 640x480 and scaled to the screen's
        // height, and the cursor with it. Scale in the ini for a setup that
        // draws the frontend at another size.
        static const float iniScale = Config::GetFloat("UI", "CursorScale", 1.0f);
        const float scale = (float)height / 480.0f * iniScale;
        Draw2D::Image(device, g_texture, floorf(x - HOT_X * scale),
                      floorf(y - HOT_Y * scale), g_w * scale, g_h * scale);
    }
}
