#pragma once

#include <string>
#include <vector>

// The mods' JavaScript runtime. One JSRuntime for everything and one JSContext
// per mod: mods cannot see each other's variables, but they share the garbage
// collector and the atom pool.
//
// All of this runs on the game thread, inside the main-loop hook. Nothing from
// CEF or any other thread touches QuickJS directly - whatever comes from the UI
// goes through the Bridge queue.
namespace Js
{
    struct Mod;

    bool Init();
    void Shutdown();

    // Loads <dir>\mod.json and runs its entry point. Returns 0 if it fails or
    // the mod is disabled.
    Mod* Load(const std::string& dir);

    const std::vector<Mod*>& Mods();
    const std::string& ModId(const Mod* mod);
    const std::string& ModDir(const Mod* mod);
    const std::string& ModUi(const Mod* mod);
    const std::string& ModName(const Mod* mod);

    // One tick: due timers plus the "frame" event for whoever subscribed.
    void Frame();

    // Native events -> JS. Arguments travel as JSON.
    void Emit(const char* event, const std::string& json);
    void EmitTo(Mod* mod, const char* event, const std::string& json);

    // speed.emit: one mod's event for the others, delivered at the start of
    // the next frame so no mod runs inside another's call.
    void Post(Mod* from, const std::string& event, const std::string& json);

    // A message from the UI, as "<modId>:<channel>". It goes only to its owner.
    void DispatchUiMessage(const std::string& channel, const std::string& json);
}
