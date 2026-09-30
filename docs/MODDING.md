# Writing mods for FRSModLoader

A mod is a folder with JavaScript in it. Nothing is compiled, nothing is
registered: FRSModLoader reads every folder under `mods\` when the game starts,
and a folder with a `mod.json` in it is a mod.

```
mods/my-mod/
  mod.json        the manifest
  main.js         logic, inside the game, on the game thread
  ui/index.html   interface, in Chromium, over the game
```

`main.js` is the only required part. A mod that only reads memory, plays a
sound or patches a byte never needs a page.

Writing in C++ instead, with an `.asi` that already hooks the game? You can
take the interface without any of this:
[NATIVE_PLUGINS.md](NATIVE_PLUGINS.md).

- [The first mod](#the-first-mod)
- [The manifest](#the-manifest)
- [The two halves](#the-two-halves)
- [Events](#events)
- [Reading the car](#reading-the-car)
- [The UI](#the-ui)
- [Console and commands](#console-and-commands)
- [Saving data](#saving-data)
- [Sound](#sound)
- [Memory and native calls](#memory-and-native-calls)
- [Drawing in the world](#drawing-in-the-world)
- [The game's own menu](#the-games-own-menu)
- [Iterating without restarting](#iterating-without-restarting)
- [Debugging](#debugging)
- [Rules of the game thread](#rules-of-the-game-thread)

## The first mod

`mods/hello/mod.json`:

```json
{
  "id": "hello",
  "name": "Hello",
  "version": "1.0.0",
  "main": "main.js",
  "enabled": true
}
```

`mods/hello/main.js`:

```js
/// <reference path="../../sdk/frsmodloader.d.ts" />

speed.print("hello from {green}" + speed.mod.name + "{/}");

speed.on("gamestate", () => {
  if (speed.game.stateName() === "gameplay") speed.print("green light");
});
```

Then:

```
.\install.ps1 -ModsOnly      copies mods\ and ui\ into the game
```

Start the game. The line shows up in the in-game console — press `/` to open
it — and in `scripts\FRSModLoader.log`.

That first line is not a comment that does nothing: it points your editor at
the SDK types, and every `speed.*` below is then completed and checked as you
type, in VS Code with nothing installed.

## The manifest

| Field | |
|---|---|
| `id` | unique; names the mod's channels, its save file and its log prefix |
| `name` | what the player sees |
| `version` | shown in the Mods menu |
| `author` | shown in the Mods menu |
| `description` | a sentence or two, shown in the Mods menu |
| `thumb` | the mod's picture in the Mods menu, relative to its folder; default `thumb.png`, 16:9 reads best |
| `main` | entry point, default `main.js` |
| `ui` | the page, e.g. `ui/index.html`; leave it out for a mod with no interface |
| `enabled` | the default: `false` keeps the mod installed and dormant until the player switches it on |
| `settings` | what the player can set in the Mods menu - see [Settings](#settings-and-the-mods-menu) |

Folders load in the order the filesystem lists them - alphabetical, in
practice - each in its own QuickJS context: a global in one mod is invisible to
another, and a mod that throws on load does not take the others down with it.

## The two halves

```
main.js  (QuickJS, game thread)        ui/index.html  (Chromium, its own process)
   |                                            |
   |-- speed.ui.send("rpm", {...}) ------------>|  frsmodloader.on("rpm", cb)
   |<------------- frsmodloader.send("ready") ---|
```

They share no memory. What crosses is JSON, so what you send has to survive
`JSON.stringify`: numbers, strings, booleans, arrays, plain objects. An address
is a number and travels fine.

`main.js` is ES modules — `import` a neighbouring file and it works:

```js
import { tank, burn } from "./fuel.js";
```

One ordering detail that costs everyone an afternoon: `main.js` runs while the
page is still mounting. Do not send state on the first frame and expect it to
land. Have the page announce itself, and answer that:

```js
// main.js
speed.on("ui:ready", () => speed.ui.send("config", { units: "metric" }));
```

```html
<!-- ui/index.html -->
<script>
  frsmodloader.on("config", (c) => { /* ... */ });
  frsmodloader.send("ready");
</script>
```

## Events

```js
speed.on("frame", () => { ... });            // every frame, on the game thread
speed.on("gamestate", (e) => e.state);       // 1 boot, 3 frontend, 4/5 loading, 6 gameplay
speed.on("keydown", (e) => e.key);           // virtual-key, only while the UI has no focus
speed.on("ui", (e) => e.channel);            // anything from the page: { channel, data }
speed.on("ui:ready", (data) => { ... });     // one channel, data delivered directly
speed.on("menu", (e) => e.id);               // an item from speed.menu was chosen
speed.off("frame", fn);                      // without fn, removes every listener
```

`setTimeout`, `setInterval` and their `clear*` exist and tick with the frame.

`"frame"` runs 60+ times a second inside the game's loop: what you put there is
part of the game's frame budget. Throttle anything the player cannot see at that
rate — a tachometer's whole logic is this:

```js
const RATE_MS = 1000 / 30;
let last = 0;

speed.on("frame", () => {
  const now = speed.now();
  if (now - last < RATE_MS) return;
  last = now;

  const t = speed.game.telemetry();
  if (t) speed.ui.send("rpm", { rpm: t.rpm, gear: t.gear, redline: t.redline });
});
```

## Reading the car

`speed.game.telemetry()` is the call to start from. It returns `null` outside a
race — which is the normal way to know you are not racing:

```js
const t = speed.game.telemetry();
if (!t) return;

t.kmh        // also .speed (m/s) and .mph
t.rpm        // 800 at idle to ~7000
t.throttle   // 0 / 0.7 / 1.0 on keyboard
t.gear       // the game counts 1 = NEUTRAL, 2 = first gear, 0 = reverse
t.boost      // negative under vacuum; null on a naturally aspirated car
t.redline    // this car's limit, 7000 or 7500
t.distance   // metres travelled
```

Around it:

```js
speed.game.stateName()   // "boot" | "frontend" | "loading" | "gameplay"
speed.game.isRacing()
speed.game.hasFocus()
speed.game.carModel()    // "350Z", "RX8", "SKYLINE" - the career car
speed.game.player()      // the raw objects, for going further
speed.game.car()         // the mirror: cheap to read, pointless to write
speed.game.physics()     // where writing has an effect
speed.game.engine()      // boost, rev limit, input throttle
```

The mirror against the physics object is the distinction worth internalising.
The car mirror is a copy the game keeps for its dial and its engine sound:
writing 300 km/h into it moves the needle and changes nothing else. Physics
reads from `speed.game.physics()`. `NOTES.md` has the field maps.

Use `carModel()` as the key for anything per-car — a table indexed by position
breaks the moment someone installs a mod that changes the car list.

## The UI

The page is not a whole document. It is a fragment mounted into its own shadow
root, with three things in scope for its `<script>` tags:

| | |
|---|---|
| `frsmodloader` | `.send(channel, data)`, `.on(channel, cb)`, `.off(channel, cb)` |
| `root` | the shadow root — use `root.getElementById`, not `document` |
| `mod` | `{ id, name, url }` |

```html
<style>
  :host { all: initial; }
  .hud { position: absolute; right: 24px; bottom: 24px; color: #fff; }
</style>

<div class="hud"><span id="rpm">0</span> rpm</div>

<script>
  const el = root.getElementById("rpm");
  frsmodloader.on("rpm", (d) => { el.textContent = Math.round(d.rpm); });
  frsmodloader.send("ready");
</script>
```

Each mod's layer covers the screen with `pointer-events: none`, so it does not
eat clicks meant for the game. Anything the player must click needs
`pointer-events: auto` on that element. The mouse needs nothing else: a click on
something your page drew is your page's, anywhere else it is the game's, and the
pointer is the game's own cursor, drawn above the UI. The **keyboard** comes to
the page only when handed over - **F1** by the player, or
`speed.ui.capture(true)` from your mod.

A screen of yours that is meant to feel like one of the game's is driven by the
arrows, Enter and Esc, and makes the game's menu sounds as it goes
(`frsmodloader.sound("down")`). The pattern, and the list of sounds, are in
[UI-KIT.md → Keyboard and sounds](UI-KIT.md#keyboard-and-sounds).

To make the panel look like the game instead of like a web page, link the kit
that ships with the loader - the green-outlined panel, the grey list, the pill
buttons, all of it in [UI-KIT.md](UI-KIT.md):

```html
<link rel="stylesheet" href="nfsu2.css">
<div class="nfs nfs-at nfs-at--br">
  <div class="nfs-panel nfs-panel--plain">
    <div class="nfs-label">Speed</div>
    <div class="nfs-readout">182</div>
  </div>
</div>
```

The href has no path in it on purpose: the shell clones your `<link>` into its
own document, so `nfsu2.css` resolves from there, at any depth.

It is a real Chromium: `fetch`, `<canvas>`, SVG, CSS animations, web fonts,
DevTools. Relative paths resolve against the mod's own `ui/` folder, so images
and extra scripts sit next to `index.html`. It is also a whole browser sharing
the frame: prefer transform and opacity for anything animated, and do not
re-layout the page sixty times a second.

## Settings and the Mods menu

The game's Options menu has a **Mods** entry, the loader's own: every installed
mod with its picture, a switch to turn it on or off, and its settings, drawn in
the game's style and driven by the keyboard like the rest of the menu. A mod
does not draw any of it. It lists what it has, in `mod.json`:

```json
{
  "id": "pops",
  "name": "Pops",
  "version": "1.2",
  "author": "Chrystian Farias",
  "description": "Pops and bangs on lift-off, with the tone of the engine.",
  "thumb": "thumb.png",
  "settings": [
    { "type": "section", "label": "Sound" },
    { "id": "volume", "type": "range", "label": "Volume",
      "min": 0, "max": 100, "step": 5, "unit": "%", "default": 80 },
    { "id": "onLift", "type": "toggle", "label": "Pops on lift-off", "default": true,
      "on": "Yes", "off": "No" },
    { "id": "engine", "type": "choice", "label": "Engine",
      "description": "Which engine's pops to play.",
      "options": [
        { "value": "rb26", "label": "RB26", "image": "legend/rb26.jpg",
          "description": "Nissan's straight six: sharp, high pops." },
        "2JZ", "SR20"
      ], "default": "rb26" },
    { "id": "test", "type": "action", "label": "Play a pop", "button": "Play" }
  ]
}
```

| `type` | row | value |
|---|---|---|
| `toggle` | two states; `on` / `off` name them | `true` / `false` |
| `range` | a bar with arrows; `min`, `max`, `step`, `unit` | a number |
| `choice` | one of `options` at a time, arrows either side; an option is a string or `{ value, label }` | the option's `value` |
| `action` | a button; `button` names it | none - it sends an event |
| `section` | a heading row between groups | none |

Every row takes a `label`. Any row may also have a **legend**: a
`description` (a sentence or two on what it does) and/or an `image` (a picture
of the effect, relative to the mod's folder; 16:9 reads best). The menu shows
the legend of whichever row has the focus - with the keys or the mouse - in a
column beside the settings, the picture on top. A `choice`'s
options may carry their own `description` and `image`; while that option is
the value, its legend replaces the row's.

The values are kept by the loader, in `scripts\FRSModLoader\data\settings\<id>.json`
- not in your folder, so an update of the mod keeps them. The mod reads them:

```js
const volume = speed.settings.get("volume");     // the value, or the default
const all = speed.settings.all();                // every one, defaults filled in

speed.on("settings", (all) => {                  // the player changed something
  applyVolume(all.volume);
});
speed.on("settings:action", (e) => {             // an action row: e.id
  if (e.id === "test") playPop();
});

speed.settings.set("volume", 60);                // from your side (a hotkey)
```

Settings decided at runtime - one row per car found in the garage, say - are
added with `speed.settings.define([...])`, the same shape as the manifest's;
they appear the next time the menu opens.

The on/off switch is the player's: it wins over the manifest's `enabled`, and
takes effect on the next start (a mod that is running keeps running until the
game restarts, and the menu says so). Native `.asi` plugins show up in the same
list by registering a manifest - see
[NATIVE_PLUGINS.md → The Mods menu](NATIVE_PLUGINS.md#the-mods-menu).

## Console and commands

The game has a console — `/` opens it — and it is where a mod talks to the
player.

```js
speed.print("fuel: {orange}12.4 L{/} left");

speed.command("fuel", (args) => {
  if (args[0] === "fill") { fill(); return "tank filled"; }
  return "fuel: " + left.toFixed(1) + " L";
}, "fuel [fill] - shows or fills the tank");
```

The handler gets the words after the command, and whatever it returns is printed
as the reply. A name belongs to the first mod that registers it, and `/help`
lists what exists with its help text. Colours are `{red}`, `{green}`,
`{yellow}`, `{orange}`, `{blue}`, `{gray}`, `{white}`, closed with `{/}`.

## Saving data

One JSON file per mod, written immediately — a game being modded crashes, and a
save that only lands on a clean exit is the one you do not have.

```js
speed.store.set("bestLap", 92.4);
speed.store.get("bestLap", 0);
speed.store.all();
```

Per career car, keyed by the game's own car id, so it survives the garage being
reordered:

```js
speed.store.car.set("odometer", km);
speed.store.car.get("odometer", 0);
speed.store.car.id();            // null with no car loaded
```

With no car loaded `get` returns the default and `set` does not write — better
no save than a save on the wrong car. The files live in
`scripts\FRSModLoader\data\<modid>.json`, outside `mods\`, so reinstalling the
mod does not wipe them.

## Sound

Sounds that answer to physics are played natively: the UI layer is a process
away, behind JSON and a message queue — right for interface sounds, wrong for
engine noises.

```js
const pop = speed.audio.load("sounds/pop1.wav");   // relative to the mod
speed.audio.play(pop, { volume: 0.8, pitch: 1.1 });
```

PCM WAV only. `pitch` is a frequency ratio (0.5 an octave down, 2 up). The pool
is 16 voices: when they are all busy, `play` returns `false` rather than cutting
another sound short.

## Memory and native calls

Everything the SDK does not wrap is still reachable. The game is 32-bit, so an
address is a plain number, and a read returns `null` instead of throwing when
the memory is not there.

```js
speed.mem.readU32(addr);
speed.mem.readF32(addr);
speed.mem.writeF32(addr, 1.5);
speed.mem.chain(0x8900AC, [0x04, 0x34, 0x18]);   // walks pointers, returns the address
speed.mem.patch(0x581470, "90 90 E9 ?? ??");     // ?? leaves that byte alone
speed.mem.nop(addr, 5);
```

```js
speed.call(0x53FEB0);                                  // cdecl, no args
speed.call(0x5FAE20, [speed.game.player()], { conv: "thiscall" });
speed.call(0x505450, ["UI_Main"], { ret: "int" });     // a string becomes char*
```

A fractional number is pushed as a float, an integer as an integer. `conv` is
`cdecl` (the default), `stdcall` or `thiscall`; `ret` is `int`, `float`,
`double`, `bool`, `string` or `void`.

This is the part that crashes the game when it is wrong, and it crashes inside
someone else's frame, where the stack says nothing. Two habits pay for
themselves: check for `null` before using an address, and write down in
`NOTES.md` how you found each one. An address with no provenance is a bug
waiting for the next person.

## Drawing in the world

`speed.ui` composites flat, over everything. For something that belongs *in* the
scene — a flame at the exhaust that shrinks with distance and disappears behind
the bodywork — there is `speed.draw`, which renders sprites inside the game's
`EndScene`, with the game's own camera:

```js
speed.draw.camera({ view: viewMatrixAddress, fov: 60 });
speed.draw.spark(x, y, z, { size: 0.25, life: 0.25, color: 0xFF8020 });
```

`attached: true` sticks the sprite to the anchor set with `speed.draw.anchor`
(the car's position and its 3x3 rotation), `vel: [x, y, z]` gives it motion, and
`color2` fades towards a second colour over its life. `speed.draw.texture`,
`textureFrom` and `atlas` point it at a texture the game already has loaded.

The view matrix address is the mod's to find and pass: the game draws through
shaders and never hands the fixed pipeline its matrices.

## The game's own menu

```js
const id = speed.menu.add({ label: "OPTIONS_TITLE" });
speed.on("menu", (e) => { if (e.id === id) speed.ui.show(); });
```

These are real `IconOption`s, built with the engine's own constructor, so they
scroll and look like the game's. The rough edge is the label: text and icon are
hashes resolved against `Languages\*.bin`, so a word of your own means adding a
string to the language file. Until then, use a key the game already has, or pass
an explicit `labelHash`.

## Iterating without restarting

The loop that costs the least time:

```
.\install.ps1 -ModsOnly      with the game running
```

then reload the page in-game. Bind it yourself while developing:

```js
speed.on("keydown", (e) => { if (e.key === 0x74) speed.ui.reload(true); });  // F5
```

`main.js` is not hot-reloaded: it runs once, on the game thread, at startup.
Changing it means restarting the game.

For the UI there is a better loop still — point `[UI] Url` in `FRSModLoader.ini`
at your dev server (`http://localhost:5173`) and keep whatever tooling you
normally use, hot reload included.

## Debugging

- `scripts\FRSModLoader.log` — `console.log` from `main.js` lands here prefixed
  with `js`, and so does the page's, through CEF's console hook. An exception in
  a mod is logged with its stack and does not stop the others.
- **`console.log` also shows up on screen**, in the same panel `speed.print`
  writes to: the line about a car appearing is worth most in the second the car
  appears, not in a file read afterwards. `[UI] Console=0` in the ini turns the
  echo off and leaves the log file alone.
- `speed.ui.devtools()` — the real Chromium DevTools, on the real page.
  `[UI] DevTools=1` in the ini opens it at startup.
- `speed.print` — the in-game console, for when the log file is too far away to
  be useful mid-corner.
- `[UI] TestPattern=1` — a checkerboard instead of the page, which tells "my CSS
  is wrong" apart from "the overlay is not compositing".

## Rules of the game thread

Five things that separate a mod that works from a mod that works on someone
else's machine:

1. **The frame belongs to the game.** Everything in `"frame"` runs before the
   game can present. Throttle, cache, and do not parse JSON in there.
2. **Every read can be `null`.** Not in a race, object not built yet, pointer
   not valid yet. Check before you dereference.
3. **The mirror is not the car.** Write to physics; read from whichever is
   cheaper. See `NOTES.md`.
4. **You are not alone.** Other `.asi` mods hook the same loop — FRSModLoader
   chains instead of replacing, and a mod that patches bytes should do the same.
5. **Namespace your own things.** Channels, command names and store keys are
   shared ground with every other mod installed.
