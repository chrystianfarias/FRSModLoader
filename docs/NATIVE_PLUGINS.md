# HTML interfaces for native `.asi` mods

Your mod is already written, in C or C++. It hooks the game itself, it knows
what it wants to show, and the only thing missing is somewhere to show it —
without learning JavaScript, rewriting anything, or giving up the hooks you
already have.

FRSModLoader keeps a Chromium running over the game. The host API lends it to
you:

```c
#include "frsmodloader.h"

static const SL_Api* sl;
static SL_Panel*     panel;

void EveryFrame(void)                 /* your existing main-loop hook */
{
    if (!sl)    { sl = SL_Connect(); return; }
    if (!panel) { panel = sl->panel_open("mymod", "MyMod\\ui.html"); return; }

    sl->panel_send_number(panel, "rpm", CurrentRpm());
}
```

```html
<!-- scripts\MyMod\ui.html -->
<style> .hud { position: absolute; right: 28px; bottom: 28px; color: #fff; } </style>
<div class="hud"><span id="rpm">0</span> rpm</div>
<script>
  frsmodloader.on("rpm", (v) => root.getElementById("rpm").textContent = Math.round(v));
</script>
```

That is the whole integration. One header, no library to link, no DLL to load:
the header finds FRSModLoader in the process and hands you a table of function
pointers.

- [Getting the header](#getting-the-header)
- [Connecting](#connecting)
- [Panels](#panels)
- [Talking to the page](#talking-to-the-page)
- [Listening to the page](#listening-to-the-page)
- [Input, and clicking things](#input-and-clicking-things)
- [The console](#the-console)
- [Threads](#threads)
- [Shipping it](#shipping-it)
- [The whole API](#the-whole-api)
- [What this is not](#what-this-is-not)

## Getting the header

Copy [`sdk/frsmodloader.h`](../sdk/frsmodloader.h) into your project. It is plain
C, depends on nothing but `windows.h`, and works from C or C++ with any
compiler that builds a 32-bit DLL.

Nothing else is needed. There is no import library: FRSModLoader is found at
runtime, so your mod loads and runs normally on a machine that does not have it
— you just do not get a panel.

## Connecting

```c
const SL_Api* sl = SL_Connect();
```

`SL_Connect` looks for `FRSModLoader.asi` in the process and asks it for the
version of the API your header describes. It returns `NULL` when FRSModLoader is
not loaded, or is older than your header.

**Call it from your loop, not from `DllMain`.** ASI loaders load mods in
whatever order the filesystem hands them over, so FRSModLoader may arrive after
you. Retrying costs a pointer comparison per frame:

```c
if (!sl) { sl = SL_Connect(); return; }
```

Opening a panel before Chromium is up is fine, by the way — the panel is
remembered and mounted the moment the UI exists.

## Panels

```c
SL_Panel* panel = sl->panel_open("mymod", "MyMod\\ui.html");
```

A panel is one page, mounted over the game, in its own shadow root — your CSS
cannot leak into another mod's panel, and no one else's can leak into yours.

- **`id`** names it: letters, digits, `-` and `_`, up to 32 characters. It is
  the prefix on every message and the name shown in the console, so use your
  mod's name. `sl` is taken by the loader, and an id already in use is refused
  (`panel_open` returns `NULL` and says so in the log).
- **`html`** is a path. A relative one resolves against the game's `scripts\`
  folder — where your `.asi` lives — so `"MyMod\\ui.html"` means
  `scripts\MyMod\ui.html`. An absolute path is taken as it is.

The page is a fragment, not a document: no `<html>`, no `<body>`, just the
styles and markup you want on screen. Three things are in scope for its
`<script>` tags:

| | |
|---|---|
| `frsmodloader` | `.send(channel, data)`, `.on(channel, cb)`, `.off(channel, cb)` |
| `root` | the shadow root — use `root.getElementById`, not `document` |
| `mod` | `{ id, name, url }` |

For the game's own look - the green-outlined panel, the grey list, the pill
buttons - link the kit the loader installs, described in [UI-KIT.md](UI-KIT.md):

```html
<link rel="stylesheet" href="nfsu2.css">
<div class="nfs nfs-panel nfs-panel--plain"> ... </div>
```

It is a real Chromium: `fetch`, `<canvas>`, SVG, animations, web fonts,
DevTools. Relative paths inside the page resolve against its own folder, so
images and extra scripts sit next to `ui.html`.

`panel_show(panel, 0)` hides it and `panel_show(panel, 1)` brings it back,
which is what you bind your toggle key to. `panel_reload(panel)` fetches the
file again: edit the HTML, press your key, see it — no restart.
`panel_close(panel)` removes it for good and the handle dies with it.

## Talking to the page

```c
sl->panel_send_number(panel, "rpm", 4200.0);        /* a JS number */
sl->panel_send_text(panel, "gear", "3rd");          /* a JS string, escaped for you */
sl->panel_send(panel, "state", "{\"lap\":2,\"best\":91.4}");  /* raw JSON you built */
```

All three arrive as `frsmodloader.on(channel, data)` in the page. Use
`panel_send` when you already have JSON; the other two exist so that a C mod
does not have to carry a JSON writer to send one number.

Messages are queued and delivered on the next frame, in order. Sending every
frame is fine — it is one small string across a pipe — but sending 60 updates a
second to something the player reads at a glance is work nobody sees. Throttle
to 20 or 30 Hz and the panel looks identical.

## Listening to the page

```c
static void __cdecl OnMessage(const char* channel, const char* json, void* user)
{
    if (!strcmp(channel, "ready"))
        sl->panel_send_text(panel, "title", "MyMod 1.0");
}

sl->panel_on(panel, OnMessage, NULL);
```

`json` is the page's data, JSON-encoded, and `"null"` when it sent nothing. A
button in the page becomes `frsmodloader.send("reset")` there and a
`strcmp(channel, "reset")` here.

One ordering detail worth knowing before it costs you an afternoon: your mod is
running long before the page mounts, and a panel can be mounted again at any
time — the player presses F5, or another mod reloads the UI. So do not push
state once and assume it landed. Have the page announce itself with
`frsmodloader.send("ready")` and answer that, which is what the example does.

## Input, and clicking things

Every panel covers the screen with `pointer-events: none`, so it never eats
clicks meant for the game. For something clickable, put `pointer-events: auto`
on that element and hand the mouse over:

```c
sl->capture_input(1);    /* keyboard and mouse go to the UI */
sl->capture_input(0);    /* and back to the game */
```

This is the same switch **F1** flips. While it is on, the game stops seeing the
keyboard (FRSModLoader blocks DirectInput too), so typing in a field does not
also drive the car.

## The console

```c
sl->print("mymod", "tank filled: {orange}12.4 L{/}");
```

One line in the in-game console, the one `/` opens. The first argument is the
tag shown in front of the line. Colours are `{red}`, `{green}`, `{yellow}`,
`{orange}`, `{blue}`, `{gray}`, `{white}` and `{#rrggbb}`, closed with `{/}`.

## The Mods menu

The game's Options menu has a **Mods** entry: every installed mod, with its
picture, an on/off switch and its settings, drawn by the loader in the game's
style. A plugin appears there by registering a manifest - the same JSON a
JavaScript mod keeps in its `mod.json`:

```c
static SL_Mod* self;

static void __cdecl OnSettings(const char* channel, const char* json, void* user)
{
    if (!strcmp(channel, "settings"))     ApplySettings(json);   /* every value */
    else if (!strcmp(channel, "action"))  RunAction(json);       /* {"id": "..."} */
    /* "enabled": the switch, true / false - see below */
}

/* once, when SL_Connect first answers */
self = sl->mod_register(
    "{ \"id\": \"spawn-car\", \"name\": \"Spawn Car\", \"version\": \"1.0\","
    "  \"description\": \"Puts any car of the game in front of you.\","
    "  \"thumb\": \"SpawnCar\\\\thumb.png\","
    "  \"settings\": ["
    "    { \"id\": \"distance\", \"type\": \"range\", \"label\": \"Distance\","
    "      \"min\": 5, \"max\": 50, \"step\": 5, \"unit\": \" m\", \"default\": 15 },"
    "    { \"id\": \"traffic\", \"type\": \"toggle\", \"label\": \"Clear traffic\", \"default\": true }"
    "  ] }");
sl->mod_on_settings(self, OnSettings, 0);
ApplySettings(sl->mod_settings(self));   /* what the player set last time */
```

- The rows (`toggle`, `range`, `choice`, `action`, `section`) are the ones in
  [MODDING.md → Settings](MODDING.md#settings-and-the-mods-menu).
- `thumb` is relative to `scripts\`, where your `.asi` lives.
- The values arrive as one JSON object. A setting the player never touched is
  not in it: its value is the `default` you declared.
- The loader keeps them in `scripts\FRSModLoader\data\settings\<id>.json`.
- The on/off switch is the player's, and an `.asi` cannot be unloaded, so it is
  yours to honour: check `mod_enabled(self)` at startup, and install no hooks
  and draw nothing when it is 0. Flipping it sends `"enabled"`; the menu tells
  the player it takes effect on the next start.

For a screen of your own driven like the game's (arrows, Enter, Esc) the
game's menu sounds are `menu_sound("down")`, and a page plays them itself with
`frsmodloader.sound("down")` - the list, and the keyboard pattern that goes
with them, are in [UI-KIT.md → Keyboard and sounds](UI-KIT.md#keyboard-and-sounds).

## Threads

Every call in the API is safe from any thread: they are queued under a lock and
applied on the game thread, inside the frame.

Your callback is the other direction, and it is always invoked **on the game
thread** — the same place your hooks run. So reading the game's memory from it
is as safe as reading it from your own hook, and you need no synchronisation of
your own for state that only those two touch.

What you must not do is block in the callback. It runs inside the game's frame:
a `Sleep`, a file read or a network call there is a stutter the player sees.

## Shipping it

```
scripts\
  MyMod.asi              your mod
  MyMod\ui.html          its page (and whatever else it loads)
  FRSModLoader.asi        not yours to ship - the player installs it
```

Say in your readme that FRSModLoader is required for the interface. Your mod
should keep working without it, since `SL_Connect` simply keeps returning
`NULL` — a missing panel is not a reason to stop hooking the game.

## The whole API

| | |
|---|---|
| `SL_Connect()` | finds FRSModLoader, returns the table or `NULL` |
| `loader_version()` | FRSModLoader's version, as a string |
| `panel_open(id, html)` | opens a panel, returns a handle |
| `panel_close(panel)` | removes it; the handle dies |
| `panel_on(panel, fn, user)` | your callback for the page's messages |
| `panel_send(panel, channel, json)` | sends raw JSON |
| `panel_send_text(panel, channel, text)` | sends a string, escaped for you |
| `panel_send_number(panel, channel, value)` | sends a double |
| `panel_reload(panel)` | re-fetches the page from disk |
| `panel_show(panel, show)` | hides or shows this panel |
| `panel_ready(panel)` | is the page mounted? |
| `print(tag, text)` | a line in the in-game console |
| `capture_input(on)` / `capturing_input()` | keyboard and mouse to the UI |
| `mod_register(manifest_json)` | v2 - puts the plugin in the Mods menu |
| `mod_settings(mod)` | v2 - the stored values, a JSON object |
| `mod_on_settings(mod, fn, user)` | v2 - `"settings"`, `"action"` and `"enabled"` from the menu |
| `mod_enabled(mod)` | v2 - whether the player left it on |
| `menu_sound(name)` | v2 - one of the game's menu sounds |

The table is versioned: `SL_Connect` passes the `SL_API_VERSION` your header
was built with, and a newer FRSModLoader answers with a table that still means
what your header says it means. Fields are only ever added at the end.

## The shape of a bigger one

The example is deliberately small. A native mod that does something
substantial - spawning cars, dressing them with parts, racing one of them -
needs nothing more from this API, and four habits:

- **It hooks the game itself.** Repoint the `CALL` at `0x581475` to your own
  function and call whoever was there, so you chain on top of FRSModLoader
  instead of fighting it — the same idea as `speed.mem.redirectCall` on the
  JavaScript side.
- **It connects from a thread, not from `DllMain`.** FRSModLoader may load
  after you, and its UI comes up a second into the game.
- **It only touches the game from the game thread**: your own frame hook and
  the panel callback, which FRSModLoader already pins there.
- **The page is just a page.** A panel written for a JavaScript mod moves to a
  native one by changing its channels and nothing else — `frsmodloader` and
  `root` mean the same thing on both sides.

Your own plugins go in `plugins\`, which git ignores. Add a target for yours
beside the guarded `SpawnCar` one in `CMakeLists.txt`, and `install.ps1` copies
the built `.asi` next to the loader, with its page in a folder of its own name.

## What this is not

This is a UI, not the JavaScript SDK. `speed.game.telemetry()`, memory reads,
native calls, per-car storage, sound and world drawing belong to mods written
in JavaScript, described in [MODDING.md](MODDING.md) — a native mod already has
all of that in C, which is the point.

If what you want is the SDK rather than the interface, write a JavaScript mod.
If you have a working `.asi` and want it to look like it belongs in 2026, this
is the door.

## The example

[`examples/asi-plugin/`](../examples/asi-plugin) is a complete mod in one file:
it connects, opens a panel, sends a counter, answers the page's `ready`, and
toggles with F9. It builds with the project (target `HelloPanel`), and it is
compiled on every build — which is also how the header stays honest.

```
build.bat nomod
copy build\Release\HelloPanel.asi   <game>\scripts\
mkdir <game>\scripts\HelloPanel
copy examples\asi-plugin\ui.html    <game>\scripts\HelloPanel\
```
