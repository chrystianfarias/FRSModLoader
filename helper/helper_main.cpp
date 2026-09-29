// FRSModLoaderHelper.exe - CEF's subprocess (renderer, gpu, utility).
//
// The game is 32-bit and CEF lives inside it, so this binary is x86 too. It
// knows nothing about NFSU2: it only hosts the renderer side of the bridge
// (RenderApp.h).

#include <windows.h>
#include "include/cef_app.h"
#include "RenderApp.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    CefMainArgs args(hInstance);
    CefRefPtr<CefApp> app;

    // Only the renderer process needs the bridge; the others (gpu, utility)
    // come up with no app at all.
    CefRefPtr<CefCommandLine> cmd = CefCommandLine::CreateCommandLine();
    cmd->InitFromString(::GetCommandLineW());
    if (cmd->GetSwitchValue("type") == "renderer")
        app = new RenderApp();

    return CefExecuteProcess(args, app, nullptr);
}
