// SpeedLoader - a modding platform for NFS Underground 2
//
// Three layers, all inside the game process:
//
//   1. native hooks    - main loop (mods) and Direct3D 9 (drawing)
//   2. QuickJS         - mod logic, on the game thread
//   3. off-screen CEF  - the UI, painted into memory and composited in EndScene
//
// DllMain only does what can safely be done under the loader lock: check the
// executable version and plant the hooks. CEF, QuickJS and the mods come up on
// the first frame - starting a thread or loading a DLL inside DllMain is asking
// for a deadlock.

#include <windows.h>

#include "bridge/Bridge.h"
#include "core/Audio.h"
#include "core/Config.h"
#include "core/Hook.h"
#include "core/Log.h"
#include "game/Game.h"
#include "game/Vehicle.h"
#include "game/EngineControl.h"
#include "game/CarFx.h"
#include "host/HostApi.h"
#include "js/JsRuntime.h"
#include "js/ModHost.h"
#include "ui/CefHost.h"
#include "ui/D3D9Hook.h"
#include "ui/InputRouter.h"
#include "ui/DInputBlock.h"
#include "ui/Overlay.h"

namespace
{
    typedef void (__cdecl* StubFn)();
    StubFn g_prevStub = 0;   // whoever was at the call site before us

    bool  g_booted = false;
    bool  g_uiEnabled = true;
    DWORD g_lastState = 0xFFFFFFFF;
    DWORD g_bootDelay = 0;

    void BootOnce()
    {
        g_booted = true;

        // Os mods carregam ANTES da UI subir, e a ordem importa: quem escreve
        // o mods.json e o ModHost, e a casca busca esse arquivo assim que a
        // pagina abre. Com o CEF primeiro, as vezes ele chegava la antes do
        // arquivo existir e a UI subia vazia, com "failed to fetch".
        if (Js::Init())
        {
            char mods[MAX_PATH], ui[MAX_PATH];
            Config::GetString("Mods", "Dir", "SpeedLoader\\mods", mods, sizeof(mods));
            char modsAbs[MAX_PATH];
            Config::Resolve(mods, modsAbs, sizeof(modsAbs));
            Config::Resolve("SpeedLoader\\ui", ui, sizeof(ui));
            ModHost::LoadAll(modsAbs, ui);
        }

        if (g_uiEnabled)
        {
            char url[512];
            Config::GetString("UI", "Url", "", url, sizeof(url));
            if (!url[0])
            {
                char shell[MAX_PATH];
                Config::Resolve("SpeedLoader\\ui\\shell.html", shell, sizeof(shell));
                _snprintf(url, sizeof(url), "file:///%s", shell);
                for (char* p = url; *p; p++) if (*p == '\\') *p = '/';
            }

            int w = 1280, h = 720;
            D3D9Hook::BackBufferSize(&w, &h);
            Overlay::SetTestPattern(Config::GetBool("UI", "TestPattern", false));

            if (CefHost::Init(url, w, h, Config::GetBool("UI", "DevTools", false)))
                Log("UI: %s (%dx%d)", url, w, h);
            else
                Log("UI off: CEF did not start");

            Bridge::Install();
        }

    }

    // Runs once per game frame, on the game thread.
    void __cdecl MainLoopHook()
    {
        __asm pushad;

        if (!g_booted)
        {
            // A moment's breath before starting Chromium: on the first frame
            // the game is still building its device and its frontend.
            if (!g_bootDelay) g_bootDelay = GetTickCount();
            if (GetTickCount() - g_bootDelay > 1000) BootOnce();
        }
        else
        {
            InputRouter::Update();
            Audio::Tick();
            D3D9Hook::Tick();
            Bridge::Drain();
            Host::Tick();

            // "/" opens the console. It is handled here and not in a mod
            // because the bar belongs to the loader: any mod can register
            // commands into it.
            if (InputRouter::TakeCommandRequest() && !InputRouter::IsCapturing())
            {
                InputRouter::SetCapturing(true);
                CefHost::SendToUi("sl:command-open", "null");
                LogIn("command bar opened");
            }

            // Keys the game does not use (it reads no keyboard messages) turn
            // into events for the mods.
            for (int vk = InputRouter::PopKey(); vk; vk = InputRouter::PopKey())
            {

                char json[48];
                _snprintf(json, sizeof(json), "{\"key\":%d}", vk);
                Js::Emit("keydown", json);
            }

            DWORD state = Game::State();
            if (state != g_lastState)
            {
                char json[64];
                _snprintf(json, sizeof(json), "{\"state\":%lu,\"previous\":%ld}",
                          state, (long)g_lastState);
                g_lastState = state;
                Js::Emit("gamestate", json);
            }

            Js::Frame();
        }

        // If another mod was already using this call site (Pops uses the same
        // one), it keeps running: we chain instead of taking its place. The
        // original target is a `ret`, so calling it is always safe.
        if (g_prevStub) g_prevStub();

        __asm popad;
    }

    void Init(HMODULE self)
    {
        Config::Init(self);
        Log("SpeedLoader 0.1.0 - NFS Underground 2 (SPEED2.EXE v1.2 NTSC)");
        Log("ini: %s", Config::g_iniPath);

        // Before the hooks: another .asi may already be loaded and ask for
        // a panel on its very first frame, which can come before ours.
        Host::Install();

        g_uiEnabled = Config::GetBool("UI", "Enabled", true);

        // Before anything else: the game calls Direct3DCreate9 early in
        // startup, and arriving after that means no way to reach the device.
        D3D9Hook::Install();
        Overlay::Init();

        // Boost and the engine object are only reachable this way: vehicle
        // physics lives in no global, it is captured when the game calls
        // 0x5A5540.
        Vehicle::Install();
        EngineControl::Install();
        CarFx::Install();

        InputRouter::Install((int)Config::GetHex("UI", "ToggleKey", VK_F1));

        // O jogo le o teclado por DirectInput, nao por mensagem de janela:
        // capturar o foco da interface nao impede as teclas de chegarem nele, e
        // digitar "/camera" trocava a camera no meio. Isto fecha a outra porta.
        DInputBlock::Install();

        uintptr_t site = (uintptr_t)Config::GetHex("Hooks", "MainLoopSite",
                                                   MAINLOOP_HOOK_SITE);
        uintptr_t prev = RedirectCall(site, (void*)MainLoopHook);
        g_prevStub = (StubFn)prev;
        Log("main loop at 0x%08X (previous target 0x%08X)", site, prev);
    }
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);

        if (!Game::VersionMatches())
        {
            MessageBoxA(NULL,
                "Incompatible SPEED2.EXE.\nUse v1.2 NTSC (4,800,512 bytes).",
                "SpeedLoader", MB_ICONERROR);
            return FALSE;
        }

        Init(self);
    }
    return TRUE;
}
