#pragma once

#include <string>

#include "../../sdk/frsmodloader.h"

// What the Mods menu shows and changes: which mods are on, and each one's
// settings.
//
// A mod declares its settings; it does not draw them. The list lives in its
// mod.json ("settings": [...]), can grow at runtime (speed.settings.define, or
// a native plugin's manifest), and the loader's page (ui/mods.html) turns it
// into rows in the game's own look. The values are the loader's to keep:
//
//   FRSModLoader\data\mods.json             { "on": [...], "off": [...] }
//   FRSModLoader\data\settings\<id>.json    { "<setting>": <value>, ... }
//
// under FRSModLoader\data, which an install never touches. The C++ side never
// looks inside a value: the page sends a mod's whole object, it is written as
// it came and handed to the mod, which parses it itself.
//
// Everything here runs on the game thread.
namespace ModSettings
{
    // `modsDir` is where the mod folders are. Before ModHost::LoadAll: loading
    // asks Enabled.
    void Init(const char* modsDir);

    // Whether a mod runs. mod.json's own "enabled" is `byDefault`; a choice
    // made in the menu wins over it, and takes effect on the next start.
    bool Enabled(const std::string& id, bool byDefault);
    void SetEnabled(const std::string& id, bool enabled);

    // The stored values, a JSON object ("{}" when there are none).
    std::string Values(const std::string& id);
    // Stores the object and, unless the mod itself set it, tells the mod.
    void SetValues(const std::string& id, const std::string& json, bool notify = true);
    // An "action" row was pressed.
    void Action(const std::string& id, const std::string& actionJson);

    // Settings a mod adds at runtime: a JSON array, appended to its list.
    void Define(const std::string& id, const std::string& schemaJson);

    // Everything the page needs, as one JSON object: every mod folder (the
    // disabled ones too) and every native plugin that registered, each with
    // its manifest, values, extra settings and whether it is running.
    std::string PanelJson();

    // ---- native plugins (sdk/frsmodloader.h, v2) ----
    SL_Mod*     RegisterNative(const char* manifestJson);
    const char* NativeValues(SL_Mod* mod);
    void        NativeOnSettings(SL_Mod* mod, SL_MessageFn fn, void* user);
    int         NativeEnabled(SL_Mod* mod);
}
