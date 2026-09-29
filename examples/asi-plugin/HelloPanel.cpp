// HelloPanel - a native .asi mod with an HTML interface, and no JavaScript.
//
// It is the smallest thing that shows the whole shape of the host API: connect
// to FRSModLoader, open a panel, push numbers into it, and answer what the page
// sends back. Build it with the project (target HelloPanel), then put
// HelloPanel.asi in the game's scripts\ folder and ui.html in
// scripts\HelloPanel\.
//
// A real mod would drive this from the game loop it already hooks. This one
// runs a thread of its own instead, precisely because it is worth showing that
// the calls are safe from anywhere: only the callback is pinned to the game
// thread, and FRSModLoader is the one that pins it.
#include <windows.h>
#include <stdio.h>

#include "frsmodloader.h"

namespace
{
    const SL_Api* g_sl = 0;
    SL_Panel*     g_panel = 0;
    bool          g_visible = true;

    // Runs on the GAME thread, inside the frame: reading the game's memory
    // from here is as safe as from your own hooks.
    void __cdecl OnMessage(const char* channel, const char* json, void*)
    {
        if (!strcmp(channel, "ready"))
        {
            // The page mounts after we are already running, so it announces
            // itself and we answer - the same handshake a JavaScript mod uses.
            g_sl->panel_send_text(g_panel, "title", "HelloPanel");
            g_sl->print("hello", "panel ready");
        }
    }

    DWORD WINAPI Run(LPVOID)
    {
        // FRSModLoader may load after us, and its UI comes up a second into the
        // game: keep asking until it answers.
        while (!(g_sl = SL_Connect())) Sleep(250);

        g_panel = g_sl->panel_open("hello", "HelloPanel\\ui.html");
        if (!g_panel) return 0;

        g_sl->panel_on(g_panel, OnMessage, 0);

        DWORD started = GetTickCount();
        bool  wasDown = false;

        for (;;)
        {
            g_sl->panel_send_number(g_panel, "uptime",
                                    (GetTickCount() - started) / 1000.0);

            // F9 hides and shows the panel. A native mod owns its own keys;
            // FRSModLoader only takes F1 (input capture) and "/" (console).
            bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
            if (down && !wasDown)
            {
                g_visible = !g_visible;
                g_sl->panel_show(g_panel, g_visible ? 1 : 0);
            }
            wasDown = down;

            Sleep(100);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        // Not SL_Connect() here: DllMain runs under the loader lock, and
        // FRSModLoader may not even be in the process yet.
        CloseHandle(CreateThread(0, 0, Run, 0, 0, 0));
    }
    return TRUE;
}
