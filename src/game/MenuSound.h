#pragma once

#include <string.h>

// The game's own menu sounds, for pages that are driven like the game's menus.
//
// 0x50D180(id) plays one through the frontend's audio object at [0x82B884];
// it does not check that the object exists, so Play does. The ids are the
// FE_SOUND_* list the executable names at 0x7F943C. The names below are what
// each is FOR, picked by ear against the game's own menus (the frs-world gas
// station started the set): a page asks for "up", not for id 3.
//
// Reachable from a page (frsmodloader.sound), a mod (speed.ui.sound) and a
// native plugin (menu_sound). Game thread only.
namespace MenuSound
{
    struct Entry { const char* name; int id; };

    static const Entry kSounds[] = {
        { "up", 3 },          { "down", 2 },          // moving between rows
        { "left", 3 },        { "right", 2 },         // moving between buttons
        { "valueLeft", 9 },   { "valueRight", 8 },    // changing a row's value
        { "confirm", 4 },                             // Enter on something that acts
        { "open", 5 },        { "close", 5 },         // a screen coming and going
        { "wrong", 19 },                              // nothing to do there
    };

    // False when the name is unknown or the frontend has no audio yet.
    inline bool Play(const char* name)
    {
        if (!name || !*(void**)0x82B884) return false;
        for (const Entry& e : kSounds)
            if (!strcmp(name, e.name))
            {
                ((void(__cdecl*)(int))0x50D180)(e.id);
                return true;
            }
        return false;
    }
}
