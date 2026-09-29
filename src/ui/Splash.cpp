#include "Splash.h"

#include "Draw2D.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Version.h"
#include "game/Game.h"
#include "js/JsRuntime.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <vector>

// ============================================================ detection
namespace
{
    const uintptr_t SPLASH_LATCH = 0x836495;

    // Candidates for the splash package. The first one that ever resolves wins
    // and the rest are dropped.
    const char* const SPLASH_NAMES[] = {
        "Chyron.fng", "Chyron", "Chyron_FE.fng",
        "MC_Bootup.fng", "UG_LS_Splash.fng", "loading_boot.fng", 0
    };

    // The menu that follows the splash. Whichever of these comes alive means
    // the splash is over.
    const char* const MENU_NAMES[] = { "UI_Main.fng", "MC_Main.fng", "ui_Main.fng", 0 };

    const DWORD POLL_MS = 120;
    // The fallback is a guess about the shape of the boot, so it gets a leash:
    // the card cannot outlive this, whatever the heuristic believes.
    const DWORD FALLBACK_MAX_MS = 60000;

    bool        g_enabled = true;
    bool        g_done = false;
    bool        g_showing = false;
    DWORD       g_start = 0;
    DWORD       g_nextPoll = 0;
    DWORD       g_dismissAt = 0;     // set when a key press should end the splash
    DWORD       g_shownAt = 0;
    DWORD       g_hiddenAt = 0;      // 0 until the card starts leaving
    const char* g_splashName = 0;    // the candidate that answered, once one does
    int         g_mods = 0;

    // The frontend is looked up this early in the boot, before every package
    // exists: a lookup that faults means "not there", not a crashed game.
    bool Alive(const char* name)
    {
        __try { return FEngFindPackage(name) != 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    const char* AnyAlive(const char* const* names)
    {
        for (; *names; names++)
            if (Alive(*names)) return *names;
        return 0;
    }

    bool Reached(DWORD now, DWORD when) { return (LONG)(now - when) >= 0; }

    void Show(DWORD now)
    {
        g_showing = true;
        g_shownAt = now;
        g_hiddenAt = 0;
        // The count is only honest once every mod has loaded, which is true
        // by the time the main loop runs.
        g_mods = (int)Js::Mods().size();
        LogGfx("splash is up, card shown (%d mods loaded)", g_mods);
    }

    void Finish(DWORD now, const char* why)
    {
        g_done = true;
        if (!g_showing) return;
        g_showing = false;
        g_hiddenAt = now;
        LogGfx("card dismissed: %s", why);
    }
}

// ============================================================ the card
//
// The card is baked once into a texture - shadow, glass, border, text, the bar's
// track, the dots - with GDI for the type, and only what moves is drawn every
// frame on top of it: the rise, the chevrons, the bar filling, the pulse and
// the sheen on the wordmark. Sizes are CSS pixels at 720 lines, scaled to the
// backbuffer.
namespace
{
    // Premultiplied float canvas, composited "over".
    struct Canvas
    {
        int w, h;
        std::vector<float> px;   // r, g, b, a per pixel

        Canvas(int width, int height) : w(width), h(height), px((size_t)width * height * 4, 0.0f) {}

        void Over(int x, int y, float r, float g, float b, float a)
        {
            if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
            float* p = &px[((size_t)y * w + x) * 4];
            const float k = 1.0f - a;
            p[0] = r * a + p[0] * k;
            p[1] = g * a + p[1] * k;
            p[2] = b * a + p[2] * k;
            p[3] = a + p[3] * k;
        }

        std::vector<uint8_t> Bgra() const
        {
            std::vector<uint8_t> out((size_t)w * h * 4);
            for (size_t i = 0; i < (size_t)w * h; i++)
            {
                const float* p = &px[i * 4];
                out[i * 4 + 0] = (uint8_t)(p[2] * 255.0f + 0.5f);
                out[i * 4 + 1] = (uint8_t)(p[1] * 255.0f + 0.5f);
                out[i * 4 + 2] = (uint8_t)(p[0] * 255.0f + 0.5f);
                out[i * 4 + 3] = (uint8_t)(p[3] * 255.0f + 0.5f);
            }
            return out;
        }
    };

    float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

    // Signed distance to a rounded rectangle centred on the origin.
    float RoundRect(float x, float y, float halfW, float halfH, float radius)
    {
        float qx = fabsf(x) - halfW + radius;
        float qy = fabsf(y) - halfH + radius;
        float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
        float inside = qx > qy ? qx : qy;
        return sqrtf(ox * ox + oy * oy) + (inside < 0.0f ? inside : 0.0f) - radius;
    }

    void Disc(Canvas& c, float cx, float cy, float radius, DWORD argb)
    {
        const float r = ((argb >> 16) & 0xFF) / 255.0f, g = ((argb >> 8) & 0xFF) / 255.0f,
                    b = (argb & 0xFF) / 255.0f, a = (argb >> 24) / 255.0f;
        for (int y = (int)(cy - radius - 1); y <= (int)(cy + radius + 1); y++)
            for (int x = (int)(cx - radius - 1); x <= (int)(cx + radius + 1); x++)
            {
                float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                c.Over(x, y, r, g, b, a * Clamp01(radius + 0.5f - d));
            }
    }

    // A line of text as coverage (0..255), antialiased by GDI in grey.
    struct Text
    {
        int w = 0, h = 0, ascent = 0;
        std::vector<uint8_t> mask;
    };

    Text Render(const wchar_t* text, int px, int weight, int spacing)
    {
        Text t;
        HDC dc = CreateCompatibleDC(NULL);
        HFONT font = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetTextCharacterExtra(dc, spacing);

        SIZE size;
        TEXTMETRICW tm;
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
        GetTextMetricsW(dc, &tm);
        t.w = size.cx;
        t.h = tm.tmHeight;
        t.ascent = tm.tmAscent;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = t.w;
        bi.bmiHeader.biHeight = -t.h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = 0;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && bits && t.w > 0 && t.h > 0)
        {
            HGDIOBJ oldBmp = SelectObject(dc, bmp);
            memset(bits, 0, (size_t)t.w * t.h * 4);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, 0, 0, text, (int)wcslen(text));
            GdiFlush();

            t.mask.resize((size_t)t.w * t.h);
            const uint8_t* p = (const uint8_t*)bits;
            for (size_t i = 0; i < t.mask.size(); i++)
            {
                uint8_t m = p[i * 4];
                if (p[i * 4 + 1] > m) m = p[i * 4 + 1];
                if (p[i * 4 + 2] > m) m = p[i * 4 + 2];
                t.mask[i] = m;
            }
            SelectObject(dc, oldBmp);
        }
        if (bmp) DeleteObject(bmp);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return t;
    }

    // Stamps `t` with its top-left at (x, y). Columns from `split` on take the
    // second colour: the wordmark is one string in two tones.
    void Stamp(Canvas& c, const Text& t, int x, int y, DWORD argb,
               int split = 0x7FFFFFFF, DWORD argb2 = 0)
    {
        for (int j = 0; j < t.h; j++)
            for (int i = 0; i < t.w; i++)
            {
                const uint8_t m = t.mask[(size_t)j * t.w + i];
                if (!m) continue;
                const DWORD col = i >= split ? argb2 : argb;
                c.Over(x + i, y + j, ((col >> 16) & 0xFF) / 255.0f,
                       ((col >> 8) & 0xFF) / 255.0f, (col & 0xFF) / 255.0f,
                       (col >> 24) / 255.0f * m / 255.0f);
            }
    }

    // Everything Draw needs to know about the baked card, in backbuffer pixels.
    struct Card
    {
        IDirect3DTexture9* texture = 0;   // the card with its shadow margin
        IDirect3DTexture9* sheen = 0;     // the wordmark again, in white
        IDirect3DTexture9* disc = 0;      // a soft white dot, for the pulse
        IDirect3DDevice9*  device = 0;
        int   height = 0;                 // backbuffer height it was baked for
        int   mods = -1;
        float s = 1.0f;                   // scale
        int   margin = 0;                 // shadow room around the card
        int   w = 0, h = 0;               // the card itself, without margin
        int   texW = 0, texH = 0;
        int   colX = 0, colW = 0;         // the text column, from the card's left
        int   barY = 0;
        int   wordX = 0, wordY = 0, wordW = 0, wordH = 0;
        float dotX = 0, dotY = 0;         // green dot centre, from the card's corner
    };
    Card g_card;

    void Release()
    {
        if (g_card.texture) g_card.texture->Release();
        if (g_card.sheen)   g_card.sheen->Release();
        if (g_card.disc)    g_card.disc->Release();
        g_card = Card();
    }

    bool Bake(IDirect3DDevice9* device, int height)
    {
        Release();

        const float s = height / 720.0f;
        auto S = [s](float v) { return (int)(v * s + 0.5f); };

        // Type. Letter-spacing as in the page: .17em for the wordmark, .13em
        // for the status line.
        Text word = Render(L"FRSMODLOADER", S(17), FW_SEMIBOLD, S(17 * 0.17f));
        Text frs  = Render(L"FRS", S(17), FW_SEMIBOLD, S(17 * 0.17f));
        wchar_t count[48], version[32];
        _snwprintf(count, 48, g_mods == 1 ? L"%d MOD LOADED" : L"%d MODS LOADED", g_mods);
        _snwprintf(version, 32, L"V%hs", FRSMODLOADER_VERSION);
        Text countT = Render(count, S(10), FW_NORMAL, S(10 * 0.13f));
        Text versionT = Render(version, S(10), FW_NORMAL, S(10 * 0.13f));

        // Layout: padding 14 20 14 16, chevrons 25 wide, a gap of 14, then the
        // column: wordmark (17), 5, bar (2), 5, status (13).
        const int statusW = S(6) + S(7) + countT.w + S(7) + S(3) + S(7) + versionT.w;
        Card k;
        k.s = s;
        k.height = height;
        k.mods = g_mods;
        k.colX = S(16 + 25 + 14);
        k.colW = word.w > statusW ? word.w : statusW;
        k.w = k.colX + k.colW + S(20);
        k.h = S(70);
        k.margin = S(48);
        k.texW = k.w + k.margin * 2;
        k.texH = k.h + k.margin * 2;
        k.barY = S(14 + 17 + 5);

        const int ox = k.margin, oy = k.margin;   // the card's corner in the canvas
        Canvas c(k.texW, k.texH);

        // Shadow (0 10px 34px, .55 black), glass (8,10,16 at .82), and a 1px
        // border (white at .10). The shadow is only outside the card, as CSS
        // draws it.
        const float halfW = k.w / 2.0f, halfH = k.h / 2.0f, radius = (float)S(14);
        const float sigma = 17.0f * s, shadowY = 10.0f * s;
        for (int y = 0; y < k.texH; y++)
            for (int x = 0; x < k.texW; x++)
            {
                const float px = x + 0.5f - ox - halfW, py = y + 0.5f - oy - halfH;
                const float d = RoundRect(px, py, halfW, halfH, radius);
                const float cover = Clamp01(0.5f - d);

                const float ds = RoundRect(px, py - shadowY, halfW, halfH, radius);
                const float shadow = 0.55f * 0.5f * erfcf(ds / (sigma * 1.41421356f));
                c.Over(x, y, 0, 0, 0, shadow * (1.0f - cover));

                c.Over(x, y, 8 / 255.0f, 10 / 255.0f, 16 / 255.0f, 0.82f * cover);
                const float ring = cover - Clamp01(0.5f - (d + 1.0f));
                c.Over(x, y, 1, 1, 1, 0.10f * ring);
            }

        // Wordmark: "FRS" white, the rest muted. `frs.w` already carries the
        // spacing after its last letter, which is where MODLOADER starts.
        k.wordX = k.colX;
        k.wordY = S(14) + (S(17) - word.h) / 2;
        k.wordW = word.w;
        k.wordH = word.h;
        Stamp(c, word, ox + k.wordX, oy + k.wordY, 0xFFFFFFFF, frs.w, 0x8CE9EEF7);

        // The bar's track; its fill is drawn live.
        for (int y = 0; y < S(2) || y < 1; y++)
            for (int x = 0; x < k.colW; x++)
                c.Over(ox + k.colX + x, oy + k.barY + y, 1, 1, 1, 0.10f);

        // Status: dot, count, separator, version, centred on a 13px line.
        const int lineY = S(14 + 17 + 5 + 2 + 5);
        const float mid = lineY + S(13) / 2.0f;
        int x = k.colX;
        k.dotX = x + S(6) / 2.0f;
        k.dotY = mid;
        Disc(c, ox + k.dotX, oy + mid, S(6) / 2.0f, 0xFF7FE0A5);
        x += S(6) + S(7);
        Stamp(c, countT, ox + x, oy + (int)(mid - countT.h / 2.0f), 0x80E9EEF7);
        x += countT.w + S(7);
        Disc(c, ox + x + S(3) / 2.0f, oy + mid, S(3) / 2.0f, 0x4DE9EEF7);
        x += S(3) + S(7);
        Stamp(c, versionT, ox + x, oy + (int)(mid - versionT.h / 2.0f), 0x80E9EEF7);

        std::vector<uint8_t> bgra = c.Bgra();
        k.texture = Draw2D::Texture(device, bgra.data(), k.texW, k.texH);

        // The sheen's copy of the wordmark: white glyphs, premultiplied.
        std::vector<uint8_t> w((size_t)word.w * word.h * 4);
        for (size_t i = 0; i < word.mask.size(); i++)
            w[i * 4 + 0] = w[i * 4 + 1] = w[i * 4 + 2] = w[i * 4 + 3] = word.mask[i];
        if (!w.empty()) k.sheen = Draw2D::Texture(device, w.data(), word.w, word.h);

        // A 64px antialiased disc, scaled to whatever the pulse needs.
        std::vector<uint8_t> d(64 * 64 * 4);
        for (int y = 0; y < 64; y++)
            for (int x2 = 0; x2 < 64; x2++)
            {
                float dist = sqrtf((x2 + 0.5f - 32) * (x2 + 0.5f - 32) + (y + 0.5f - 32) * (y + 0.5f - 32));
                uint8_t a = (uint8_t)(Clamp01(32.0f - dist) * 255.0f);
                uint8_t* p = &d[(y * 64 + x2) * 4];
                p[0] = p[1] = p[2] = p[3] = a;
            }
        k.disc = Draw2D::Texture(device, d.data(), 64, 64);

        k.device = device;
        g_card = k;
        if (!g_card.texture)
        {
            LogGfx("card: could not create its texture");
            return false;
        }
        LogGfx("card baked: %dx%d at %.2fx", k.w, k.h, s);
        return true;
    }

    // cubic-bezier(.2, .8, .3, 1) and friends, near enough for 300 ms.
    float EaseOut(float t)   { t = Clamp01(t); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
    float EaseInOut(float t) { t = Clamp01(t); return t * t * (3.0f - 2.0f * t); }

    DWORD Mix(DWORD a, DWORD b, float t)
    {
        DWORD out = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
            out |= (DWORD)(ca + (cb - ca) * t + 0.5f) << shift;
        }
        return out;
    }

    // One chevron, a block with a chevron cut out of it: two convex quads.
    void Chevron(IDirect3DDevice9* device, float x, float y, float w, float h, float opacity)
    {
        const DWORD top = 0xFF6FD3FF, bottom = 0xFF2D7FF0, middle = Mix(top, bottom, 0.5f);
        const float a[8] = { x, y,  x + w * 0.55f, y,  x + w, y + h * 0.5f,  x + w * 0.45f, y + h * 0.5f };
        const DWORD ca[4] = { top, top, middle, middle };
        const float b[8] = { x + w * 0.45f, y + h * 0.5f,  x + w, y + h * 0.5f,  x + w * 0.55f, y + h,  x, y + h };
        const DWORD cb[4] = { middle, middle, bottom, bottom };
        Draw2D::Polygon(device, a, ca, 4, opacity);
        Draw2D::Polygon(device, b, cb, 4, opacity);
    }
}

namespace Splash
{
    void Init()
    {
        g_enabled = Config::GetBool("UI", "SplashCard", true);
        if (!g_enabled) g_done = true;
    }

    void Tick()
    {
        if (g_done) return;

        const DWORD now = GetTickCount();
        if (!g_start) g_start = now;

        // Once the game is loading or racing there is no splash to wait for.
        if (Game::State() >= 4) { Finish(now, "the game is loading"); return; }

        if (!Reached(now, g_nextPoll)) return;
        g_nextPoll = now + POLL_MS;

        const char* menu = AnyAlive(MENU_NAMES);
        if (menu) { Finish(now, menu); return; }

        bool onSplash;
        if (g_splashName)
        {
            // The package alone is not enough: Chyron_FE stays loaded into the
            // menu. The menu taking over is what ends the splash (above).
            onSplash = Alive(g_splashName);
        }
        else if (const char* hit = AnyAlive(SPLASH_NAMES))
        {
            g_splashName = hit;
            LogGfx("splash package is \"%s\" - using it from now on", hit);
            onSplash = true;
        }
        else
        {
            // Fallback: the splash is ready (latch) and the menu has not taken
            // over. "Press a button" is literally how this screen ends, so a
            // key press means it is on its way out even before the menu is up.
            onSplash = *(volatile BYTE*)SPLASH_LATCH == 1;
            if (g_dismissAt && Reached(now, g_dismissAt)) onSplash = false;
            if (now - g_start > FALLBACK_MAX_MS) onSplash = false;
        }

        if (onSplash && !g_showing) Show(now);
        else if (!onSplash && g_showing) Finish(now, "the splash is gone");
    }

    void OnKey()
    {
        // In fallback mode, any key while the card is up is the button that
        // dismisses the splash. Give the screen a moment to change first.
        if (!g_done && g_showing && !g_splashName && !g_dismissAt)
            g_dismissAt = GetTickCount() + 400;
    }

    bool Visible()
    {
        return g_showing || (g_hiddenAt && GetTickCount() - g_hiddenAt < 300);
    }

    void Draw(IDirect3DDevice9* device, int width, int height)
    {
        if (!Visible())
        {
            // Gone for good: the textures have nothing left to do.
            if (g_done && g_card.texture) Release();
            return;
        }
        if (height <= 0) return;

        if (!g_card.texture || g_card.device != device || g_card.height != height ||
            g_card.mods != g_mods)
        {
            if (!Bake(device, height))
            {
                g_showing = false;
                g_hiddenAt = 0;
                g_done = true;
                return;
            }
        }
        const Card& k = g_card;
        const float s = k.s;

        // In: rise 14px and fade in over .3s. Out: the same, backwards.
        const DWORD now = GetTickCount();
        const float in = g_hiddenAt ? 1.0f - (now - g_hiddenAt) / 300.0f
                                    : (now - g_shownAt) / 300.0f;
        const float e = EaseOut(in);
        const float opacity = e;
        const float x = floorf(40.0f * s);
        const float y = floorf(height - 40.0f * s - k.h + (1.0f - e) * 14.0f * s);
        const float t = (now - g_shownAt) / 1000.0f;   // seconds on screen

        Draw2D::Image(device, k.texture, x - k.margin, y - k.margin,
                      (float)k.texW, (float)k.texH, 0xFFFFFFFF, opacity);

        // Chevrons: three speed lines running left to right, staggered .12s,
        // 1.15s a lap. Dim at rest, bright and 2px on at the middle of it.
        const float chevW = 7 * s, chevH = 15 * s, chevY = y + (k.h - chevH) / 2.0f;
        for (int i = 0; i < 3; i++)
        {
            float local = t - i * 0.12f, p = 0.0f;
            if (local >= 0.0f)
            {
                float lap = fmodf(local, 1.15f) / 1.15f;
                p = lap < 0.35f ? lap / 0.35f : lap < 0.7f ? 1.0f - (lap - 0.35f) / 0.35f : 0.0f;
                p = EaseInOut(p);
            }
            Chevron(device, x + 16 * s + i * 9 * s + 2 * s * p, chevY, chevW, chevH,
                    (0.22f + 0.78f * p) * opacity);
        }

        // The bar fills once, .85s after a .15s wait: the "loaded" gesture.
        const float fill = EaseOut((t - 0.15f) / 0.85f);
        if (fill > 0.0f)
            Draw2D::Rect(device, x + k.colX, y + k.barY, k.colW * fill,
                         (float)(int)(2 * s + 0.5f), 0xFF2D7FF0, 0xFF6FD3FF, opacity);

        // The dot's pulse: a ring that grows 7px and fades, every 1.8s.
        if (k.disc)
        {
            const float lap = fmodf(t, 1.8f) / 1.8f;
            const float grow = Clamp01(lap / 0.7f);
            const float r = 3 * s + 7 * s * grow;
            Draw2D::Image(device, k.disc, x + k.dotX - r, y + k.dotY - r, r * 2, r * 2,
                          0xFF7FE0A5, 0.55f * (1.0f - grow) * opacity);
        }

        // The sheen: a band of light through the wordmark's letters, once the
        // card has landed (.5s), 3.2s a lap, crossing in the first 55% of it.
        // Drawn as thin scissored slices of a white copy of the wordmark, each
        // as bright as the band is at that column.
        const float sheenT = t - 0.5f;
        if (k.sheen && sheenT > 0.0f)
        {
            const float lap = fmodf(sheenT, 3.2f) / 3.2f;
            if (lap < 0.55f)
            {
                const float W = (float)k.wordW;
                const float pos = 1.3f - 1.6f * EaseInOut(lap / 0.55f);   // 130% -> -30%
                const float centre = -1.2f * W * pos + 1.1f * W;
                const float half = 0.176f * W;
                const float wx = x + k.wordX, wy = y + k.wordY;

                RECT saved;
                device->GetScissorRect(&saved);
                device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                const int slices = 12;
                for (int i = 0; i < slices; i++)
                {
                    const float a0 = centre - half + 2 * half * i / slices;
                    const float a1 = centre - half + 2 * half * (i + 1) / slices;
                    const float mid = (a0 + a1) / 2.0f;
                    const float glow = 0.95f * (1.0f - fabsf(mid - centre) / half);
                    RECT r = { (LONG)(wx + a0), (LONG)wy, (LONG)(wx + a1), (LONG)(wy + k.wordH) };
                    if (r.right <= r.left || glow <= 0.0f) continue;
                    device->SetScissorRect(&r);
                    Draw2D::Image(device, k.sheen, wx, wy, W, (float)k.wordH,
                                  0xFFFFFFFF, glow * opacity);
                }
                device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
                device->SetScissorRect(&saved);
            }
        }
    }
}
