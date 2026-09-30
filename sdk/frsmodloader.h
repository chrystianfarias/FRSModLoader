/*
 * FRSModLoader host API - an HTML/CSS interface for a native .asi mod.
 *
 * This is for mods written in C or C++, that already hook the game themselves
 * and only want a real interface over it: a panel of HTML, rendered by the
 * Chromium layer FRSModLoader already keeps running, talking to your code by
 * message. No JavaScript mod, no mod.json, no QuickJS involved.
 *
 *     #include "frsmodloader.h"
 *
 *     static SL_Panel*   panel;
 *     static const SL_Api* sl;
 *
 *     static void __cdecl OnUi(const char* channel, const char* json, void* user)
 *     {
 *         if (!strcmp(channel, "ready"))
 *             sl->panel_send_text(panel, "hello", "MyMod 1.0");
 *     }
 *
 *     // From your main-loop hook, NOT from DllMain: FRSModLoader may not be
 *     // loaded yet when your DLL is, and its UI comes up a second later.
 *     void EveryFrame(void)
 *     {
 *         if (!sl) { sl = SL_Connect(); return; }
 *         if (!panel) {
 *             panel = sl->panel_open("mymod", "MyMod\\ui.html");
 *             sl->panel_on(panel, OnUi, 0);
 *         }
 *         sl->panel_send_number(panel, "rpm", CurrentRpm());
 *     }
 *
 * Inside the page, `frsmodloader`, `root` and `mod` are in scope exactly as they
 * are for a JavaScript mod:
 *
 *     <div class="hud">rpm <span id="v">0</span></div>
 *     <script>
 *       frsmodloader.on("rpm", (v) => root.getElementById("v").textContent = v);
 *       frsmodloader.send("ready");
 *     </script>
 *
 * Threading: every call here is safe from any thread. Your callback, though, is
 * always invoked on the GAME thread, inside the frame - the same place your
 * hooks run, so reading the game's memory from it is safe.
 *
 * FRSModLoader has its own terms of use (see LICENSE), but this header is the
 * interface to it: use it in your own mod freely.
 */
#ifndef FRSMODLOADER_H_
#define FRSMODLOADER_H_

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL_API_VERSION 2

/* Version 2 added the mod_* calls (the Mods menu) and menu_sound. A plugin
 * built against version 1 gets the same table and simply does not see them. */

typedef struct SL_Panel SL_Panel;
typedef struct SL_Mod SL_Mod;

/* A message from the page: `channel` is what the page passed to
 * frsmodloader.send, `json` is its data, JSON-encoded ("null" when it sent
 * nothing). Runs on the game thread. */
typedef void(__cdecl* SL_MessageFn)(const char* channel, const char* json, void* user);

typedef struct SL_Api
{
    unsigned version;                        /* SL_API_VERSION of this table */
    const char*(__cdecl* loader_version)(void);

    /* Opens a panel: `id` names it (letters, digits, '-' and '_'), `html` is
     * the page. A relative path resolves against the game's scripts\ folder,
     * where your .asi lives, so "MyMod\\ui.html" is the usual form.
     *
     * Returns NULL only if the id is unusable or already taken. The panel is
     * mounted as soon as the UI is up, so calling this early is fine. */
    SL_Panel*(__cdecl* panel_open)(const char* id, const char* html);

    /* Takes the panel off the screen and forgets it. The pointer dies here. */
    void(__cdecl* panel_close)(SL_Panel* panel);

    /* Your callback for frsmodloader.send from the page. One per panel; passing
     * NULL removes it. */
    void(__cdecl* panel_on)(SL_Panel* panel, SL_MessageFn fn, void* user);

    /* Sends to the page, where frsmodloader.on(channel, cb) receives it.
     *
     *   _send       `json` is raw JSON you built: "42", "{\"a\":1}", "null".
     *   _send_text  a string, escaped and quoted for you.
     *   _send_number a double, which arrives as a JavaScript number.
     */
    void(__cdecl* panel_send)(SL_Panel* panel, const char* channel, const char* json);
    void(__cdecl* panel_send_text)(SL_Panel* panel, const char* channel, const char* text);
    void(__cdecl* panel_send_number)(SL_Panel* panel, const char* channel, double value);

    /* Re-fetches the page from disk - the loop for iterating on HTML without
     * restarting the game. */
    void(__cdecl* panel_reload)(SL_Panel* panel);

    /* Shows or hides this panel alone. Panels start visible. */
    void(__cdecl* panel_show)(SL_Panel* panel, int show);

    /* Is the panel on screen, with its page mounted? */
    int(__cdecl* panel_ready)(SL_Panel* panel);

    /* A line in the in-game console (the one "/" opens). `tag` is the name
     * shown in front of it; NULL uses the panel ids you opened, or "sl". */
    void(__cdecl* print)(const char* tag, const char* text);

    /* Hands keyboard and mouse to the UI, and takes them back - the same thing
     * F1 does. Anything the player must click needs this on. */
    void(__cdecl* capture_input)(int on);
    int(__cdecl* capturing_input)(void);

    /* ---- v2: the Mods menu ------------------------------------------------
     *
     * The game's Options menu has a "Mods" entry: the list of installed mods,
     * each with its picture, an on/off switch and its settings, drawn by the
     * loader in the game's own style. A plugin appears there by registering a
     * manifest - the same JSON a JavaScript mod keeps in mod.json:
     *
     *   {
     *     "id": "spawn-car", "name": "Spawn Car", "version": "1.0",
     *     "author": "...", "description": "...",
     *     "thumb": "SpawnCar\\thumb.png",           (relative to scripts\)
     *     "settings": [
     *       { "type": "section", "label": "Spawning" },
     *       { "id": "distance", "type": "range", "label": "Distance",
     *         "min": 5, "max": 50, "step": 5, "unit": " m", "default": 15 },
     *       { "id": "traffic", "type": "toggle", "label": "Clear traffic", "default": true },
     *       { "id": "color", "type": "choice", "label": "Colour",
     *         "options": ["Stock", { "value": "rand", "label": "Random" }], "default": "Stock" },
     *       { "id": "reset", "type": "action", "label": "Reset to defaults" }
     *     ]
     *   }
     *
     * The loader stores the values (FRSModLoader\data\settings\<id>.json)
     * and hands them over as one JSON object. Settings the player never
     * touched are absent from it: their value is the "default" you declared.
     *
     * Call these from the game thread (your hooks, your callbacks) or once at
     * startup. */

    /* Puts the plugin in the menu. Registering the same id again replaces the
     * manifest. NULL when the JSON has no usable "id". */
    SL_Mod*(__cdecl* mod_register)(const char* manifest_json);

    /* The stored values, a JSON object ("{}" when there are none). The pointer
     * stays valid until the values change. */
    const char*(__cdecl* mod_settings)(SL_Mod* mod);

    /* Your callback for what the player does in the menu, on the game thread:
     *
     *   channel "settings"  json = every stored value, the whole object
     *   channel "action"    json = {"id": "<the action row>"}
     *   channel "enabled"   json = true / false - the switch, which takes
     *                       effect on the next start: a plugin that is off
     *                       should stand down then (see mod_enabled) */
    void(__cdecl* mod_on_settings)(SL_Mod* mod, SL_MessageFn fn, void* user);

    /* Whether the player left the plugin on. An .asi cannot be unloaded, so
     * honouring this - installing no hooks, drawing nothing - is up to you. */
    int(__cdecl* mod_enabled)(SL_Mod* mod);

    /* One of the game's own menu sounds, by what it is for: "up", "down",
     * "left", "right", "valueLeft", "valueRight", "confirm", "open", "close",
     * "wrong". 0 when the name is unknown or the frontend has no audio yet.
     * Game thread only. A page plays them itself: frsmodloader.sound(name). */
    int(__cdecl* menu_sound)(const char* name);
} SL_Api;

typedef const SL_Api*(__cdecl* SL_GetApiFn)(unsigned version);

/* Finds FRSModLoader in the process. Returns NULL when it is not loaded (yet),
 * or when it is older than the version this header describes - so call it
 * every frame until it answers, rather than once at startup. */
static __inline const SL_Api* SL_Connect(void)
{
    HMODULE module = GetModuleHandleA("FRSModLoader.asi");
    SL_GetApiFn get;

    if (!module) return 0;

    get = (SL_GetApiFn)GetProcAddress(module, "FRSModLoader_GetApi");
    return get ? get(SL_API_VERSION) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* FRSMODLOADER_H_ */
