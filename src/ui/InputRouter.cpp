#include "InputRouter.h"

#include "CefHost.h"
#include "Overlay.h"
#include "core/Log.h"
#include "game/Game.h"

namespace
{
    WNDPROC g_origWndProc = 0;
    HWND    g_hwnd = 0;
    bool    g_capturing = false;
    bool    g_mouse = true;          // with the keyboard, the mouse too
    bool    g_commandAsked = false;  // a "/" was typed; see TakeCommandRequest
    bool    g_overUi = false;        // the pointer is over something the page drew
    int     g_uiButtons = 0;         // buttons pressed on the page, still down
    int     g_toggleKey = VK_F1;

    // Circular key queue for mods. Sixteen is plenty: the main loop drains it
    // every frame, and losing a key from someone who typed 17 times in 16 ms is
    // not a real problem.
    int  g_keys[16];
    int  g_keyHead = 0, g_keyTail = 0;

    void PushKey(int vk)
    {
        int next = (g_keyHead + 1) % 16;
        if (next == g_keyTail) return;   // full: drop the newest
        g_keys[g_keyHead] = vk;
        g_keyHead = next;
    }

    // The UI is painted at backbuffer size; the window may be another size
    // (border, DPI, stretched fullscreen). Convert before handing it to CEF.
    void ToUiCoords(LPARAM lParam, int* x, int* y)
    {
        int cx = (short)LOWORD(lParam);
        int cy = (short)HIWORD(lParam);

        RECT rc;
        int uiW = 0, uiH = 0;
        Overlay::Size(&uiW, &uiH);
        if (GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0 &&
            uiW > 0 && uiH > 0)
        {
            cx = MulDiv(cx, uiW, rc.right);
            cy = MulDiv(cy, uiH, rc.bottom);
        }
        *x = cx;
        *y = cy;
    }

    // The page draws over the whole screen, mostly transparent. Where it drew
    // something the mouse is the page's; where the game shows through, the
    // game's. Faint pixels (a panel's shadow fading out) count as the game.
    bool OverUi(int x, int y)
    {
        return Overlay::IsVisible() && CefHost::AlphaAt(x, y) >= 16;
    }

    // Whether the page takes a mouse event at (x, y): every one while the UI
    // holds the mouse (F1); otherwise the ones over the page, and the rest of
    // a press that started on it.
    bool UiTakesMouse(int x, int y)
    {
        return (g_capturing && g_mouse) || g_uiButtons || OverUi(x, y);
    }

    // Mouse messages, in either mode. Returns true when the page took it.
    bool RouteMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        int x, y;
        switch (msg)
        {
        case WM_MOUSEMOVE:
            // Every move reaches the page, so hover comes and goes as it
            // should; the game reads the pointer with GetCursorPos anyway.
            ToUiCoords(lParam, &x, &y);
            g_overUi = OverUi(x, y);
            CefHost::MouseMove(x, y, (wParam & MK_LBUTTON) != 0);
            return g_capturing && g_mouse;

        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        {
            ToUiCoords(lParam, &x, &y);
            const int button = (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                             : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 2 : 0;
            const bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                               msg == WM_MBUTTONDOWN);
            // A release belongs to whoever got the press.
            const bool mine = down ? UiTakesMouse(x, y)
                                   : (g_uiButtons & (1 << button)) || (g_capturing && g_mouse);
            if (!mine) return false;

            if (down) g_uiButtons |= 1 << button;
            else      g_uiButtons &= ~(1 << button);
            if (down) SetCapture(hwnd);
            else if (!g_uiButtons) ReleaseCapture();
            CefHost::MouseButton(x, y, button, down, 1);
            return true;
        }

        case WM_MOUSEWHEEL:
        {
            POINT p = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            ScreenToClient(hwnd, &p);
            ToUiCoords(MAKELPARAM(p.x, p.y), &x, &y);
            if (!UiTakesMouse(x, y)) return false;
            CefHost::MouseWheel(x, y, GET_WHEEL_DELTA_WPARAM(wParam));
            return true;
        }

        case WM_MOUSELEAVE:
            g_overUi = false;
            CefHost::MouseLeave();
            return g_capturing && g_mouse;
        }
        return false;
    }

    LRESULT CALLBACK WndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        // WM_SYSKEYDOWN counts as a key press too. Windows sends F10 and
        // anything held with Alt down that path, not WM_KEYDOWN, so a mod that
        // binds F10 would wait forever for a message that never comes - which
        // is exactly what happened the first time one did.
        const bool keyDown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);

        if (keyDown && (int)wParam == g_toggleKey)
        {
            InputRouter::SetCapturing(!g_capturing);
            return 0;
        }

        // "/" opens the command bar, and it is the character that counts, not
        // the key: on a Brazilian (ABNT2) keyboard the key where a US one has
        // "/" (VK_OEM_2) types ";", and "/" is VK_ABNT_C1 or the keypad's.
        if (msg == WM_CHAR && wParam == '/' && !g_capturing && !(lParam & (1 << 30)))
        {
            g_commandAsked = true;
            return 0;
        }

        // Auto-repeat from a held key (bit 30) is of no interest to mods.
        if (keyDown && !g_capturing && !(lParam & (1 << 30)))
            PushKey((int)wParam);

        // Swallow the F10 that got this far: left alone, DefWindowProc opens
        // the window menu and the game loses focus mid-race.
        if (msg == WM_SYSKEYDOWN && (int)wParam == VK_F10) return 0;

        // No Windows arrow over the game, UI or not: the pointer is the
        // game's own, drawn above the page by Cursor.
        if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(NULL);
            return TRUE;
        }

        // The mouse needs no F1: the page gets what lands on it.
        if (RouteMouse(hwnd, msg, wParam, lParam)) return 0;

        // The keyboard does: F1 (or a mod asking for it) hands it over.
        if (g_capturing)
        {
            switch (msg)
            {
            case WM_KEYDOWN: case WM_KEYUP:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR: case WM_SYSCHAR:
                CefHost::Key(msg, wParam, lParam);
                return 0;
            }
        }

        if (msg == WM_KILLFOCUS) CefHost::SetFocus(false);
        if (msg == WM_SETFOCUS && g_capturing) CefHost::SetFocus(true);

        return CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
    }
}

namespace InputRouter
{
    void Install(int toggleVirtualKey)
    {
        g_toggleKey = toggleVirtualKey;
    }

    void Update()
    {
        if (g_origWndProc)
        {
            // The page changes under a mouse that stands still too: a panel
            // opening under the pointer has to take the next click.
            POINT p;
            RECT rc;
            if (GetCursorPos(&p) && ScreenToClient(g_hwnd, &p) &&
                GetClientRect(g_hwnd, &rc) && PtInRect(&rc, p))
            {
                int x, y;
                ToUiCoords(MAKELPARAM(p.x, p.y), &x, &y);
                g_overUi = OverUi(x, y);
            }
            else g_overUi = false;
            return;
        }

        HWND hwnd = Game::Window();
        if (!hwnd || !IsWindow(hwnd)) return;

        g_hwnd = hwnd;
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                                   (LONG_PTR)WndProcHook);
        LogIn("window 0x%p hooked (original 0x%p), UI key = 0x%02X",
              hwnd, g_origWndProc, g_toggleKey);
    }

    void Shutdown()
    {
        if (g_origWndProc && g_hwnd && IsWindow(g_hwnd))
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
        g_origWndProc = 0;
    }

    bool IsCapturing() { return g_capturing; }
    bool IsCapturingMouse() { return g_capturing && g_mouse; }
    bool MouseOnUi() { return (g_capturing && g_mouse) || g_uiButtons || g_overUi; }

    bool TakeCommandRequest()
    {
        const bool asked = g_commandAsked;
        g_commandAsked = false;
        return asked;
    }

    int PopKey()
    {
        if (g_keyTail == g_keyHead) return 0;
        int vk = g_keys[g_keyTail];
        g_keyTail = (g_keyTail + 1) % 16;
        return vk;
    }

    void SetCapturing(bool capturing, bool mouse)
    {
        if (capturing == g_capturing && (!capturing || mouse == g_mouse)) return;
        const bool wasMouse = g_capturing && g_mouse;
        g_capturing = capturing;
        g_mouse = mouse;

        CefHost::SetFocus(capturing);
        // Windows' cursor stays as the game left it, hidden. The one the UI
        // shows is the game's own, drawn over the page (see Cursor).
        const bool wantMouse = capturing && mouse;
        if (wasMouse && !wantMouse)
        {
            g_uiButtons = 0;
            ReleaseCapture();
            CefHost::MouseLeave();
        }
        LogIn("UI capture: %s", !capturing ? "off" : mouse ? "on" : "on (keyboard only)");
    }
}
