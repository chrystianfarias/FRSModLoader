#pragma once

#include <windows.h>

#include "quickjs.h"
#include <string>
#include <vector>

// Internal to the JS layer: what the Api*.cpp files need to see of the runtime.
// Mod lives here, rather than in JsRuntime.h, so the rest of the project never
// depends on QuickJS.
namespace Js
{
    struct Timer
    {
        int      id;
        DWORD    due;        // GetTickCount when it fires
        int      interval;   // 0 = setTimeout
        JSValue  fn;
        bool     dead;
    };

    struct Mod
    {
        std::string id, name, dir, main, ui;
        bool        enabled = true;
        JSContext*  ctx = nullptr;
        JSValue     handlers;        // { event: [fn, ...] }
        std::vector<Timer> timers;
        int         nextTimerId = 1;
    };

    // The mod that owns a context (JS_GetContextOpaque).
    Mod* Owner(JSContext* ctx);

    // Pending exception -> log, with the mod's name. Does not rethrow.
    void ReportException(JSContext* ctx, const char* what);

    // Each Api*.cpp hangs its piece off `speed`.
    void RegisterCore(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterMemory(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterGame(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterUi(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterAudio(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterEngine(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterCommands(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterStore(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterDraw(JSContext* ctx, JSValue speed, Mod* mod);
    void RegisterSettings(JSContext* ctx, JSValue speed, Mod* mod);

    // Slash commands: the bar in the shell sends a line, this runs it.
    void RunCommand(const std::string& line);
    void ClearCommands(JSContext* ctx);
}
