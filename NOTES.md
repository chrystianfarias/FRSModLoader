# FRSModLoader — engineering notes

A modding platform for NFS Underground 2: JavaScript calling native code, and a
Chromium layer composited over the game. Target: `SPEED2.EXE` v1.2 NTSC
(4,800,512 bytes, image base `0x400000`).

The game addresses came from the author's earlier `Pops` project, which in turn
built on NFSU2ExtraOptions. Those notes remain the reverse-engineering
reference; this file only covers decisions specific to this platform.

## Architecture decisions

**Everything in one process.** CEF renders off-screen (`OnPaint` hands us a
BGRA buffer) and the frame is composited as a full-screen quad inside the
game's `EndScene`. The alternative — a transparent window on top — would be far
less code, but it only works in windowed mode and disappears in fullscreen.

**QuickJS, not V8.** Mod code runs on the game thread, in sync with the frame.
That is what lets `speed.on("frame", ...)` react per frame and `speed.call`
enter a native function with no cross-process round trip. V8 on 32-bit would be
a build problem on its own.

**JSON between the two halves.** `main.js` (QuickJS) and the page (V8) share
nothing; the bridge is a `CefProcessMessage` carrying a JSON string. It is the
lowest common denominator between two different engines and avoids type
marshalling.

**Native mods borrow the UI, they do not get a second one.** The host API
(`src/host/HostApi.cpp`, exported as `FRSModLoader_GetApi`) hands an .asi mod a
panel mounted by the same shell, with the same `<id>:<channel>` convention a
JavaScript mod's page uses. The alternative - a separate surface for native
plugins - would have meant a second mounting path in the shell, a second set of
z-order rules and two ways for a page to be written. As it is, the shell gained
three messages (`sl:mount`, `sl:unmount`, `sl:panel-show`) and nothing else.

Native panels exist only in memory: they are not in `mods.json`, because the
mod that owns one is a DLL, not a folder. That is why the shell asks for them
back with `sl:panels` every time it loads, F5 included.

The API is queued on both sides. A plugin is somebody else's DLL and may call
from any thread, so calls land in a queue and `Host::Tick` drains it on the
game thread; messages the other way arrive through `Bridge::Drain`, which means
a plugin's callback runs where its own hooks run and can read game memory
without ceremony.

**The helper is a separate executable.** Chromium requires one for its
subprocesses (renderer, gpu, utility). `FRSModLoaderHelper.exe` is x86 like the
game and knows nothing about NFSU2: it only hosts the renderer side of the
bridge.

**The UI's art comes out of the game, at runtime.** `src/ui/GameTextures.cpp`
reads the texture packs (bChunk `B3300000`, entries in `33310004`, pixels in
`33320002` from the next 128-byte boundary; `.LZC` is JDLZ and is inflated
whole) and serves any texture as a PNG at `http://nfsu2.tex/<name>`, through a
scheme handler factory on the standard `http` scheme - nothing to register in
the helper. Formats seen: DXT1, DXT3, A8R8G8B8 and P8, whose 1 KB BGRA palette
sits at the start of the data block (`+0x34` of the entry). Extracting them to
PNGs in the repository would have been simpler and is not ours to do; reading
them live also means a texture mod shows up in the UI.

## What works

Confirmed running (log at `scripts\FRSModLoader.log`):

- IAT hook on `Direct3DCreate9` -> `CreateDevice` -> device vtable (`EndScene`
  42, `Reset` 16). The device is captured at 0.17 s, before the frontend.
- CEF starts inside the game with `multi_threaded_message_loop`, creates the
  off-screen browser and paints: `first OnPaint: 1280x800`.
- `shell.html` loads, reads `mods.json` and mounts each mod's UI in its own
  shadow root.
- QuickJS loads the mods, runs their `main.js` and delivers gameflow events
  (`3 -> 4 -> 5 -> 6` when entering a race).
- The bridge works both ways: the page sends `ready` and the mod receives it.
- The game's WndProc is hooked (`0x5CCD60`), with a configurable UI key.
- Chaining at `0x581475`: Pops keeps running alongside (its hook shows up in
  the log as the previous target, inside `Pops.asi`).

## The D3D9 vtable war — solved

The first frame drew and then the UI vanished. The log told the story:
`EndScene alive: 1 frames` and never again, while the main-loop hook kept
running. The diagnostic that cracked it was printing **which module** the
vtable slot had come to point at:

```
[gfx] vtable EndScene is now 0x6AC26720 (C:\WINDOWS\SYSTEM32\d3d9.dll),
      not ours 0x68E9AC90
```

So: something **restores** the device vtable to the original a few seconds
after we install — almost certainly `NFSUnderground2.WidescreenFix.asi` or
`NFSU2HDReflections.asi`, which load earlier and also touch D3D9.

Fighting over a vtable slot is a losing game: last writer wins, and that writer
keeps rewriting. The fix was to **detour the function body inside d3d9.dll**,
which nobody restores:

```
[gfx] EndScene detoured in code (0x6AC26720, 7 bytes), immune to vtable swaps
[gfx] Reset detoured in code (0x6ACA09D0, 5 bytes), immune to vtable swaps
[gfx] first quad drawn (1280x800)
[gfx] EndScene alive: 3346 frames
```

Two consequences:

- The Windows 11 `d3d9.dll` does **not** have the classic hot-patch prologue
  (`8B FF 55 8B EC`); it starts with `push 0x14; mov eax, imm32`. Hence
  `CopyLength` in `core/Hook.h`: it sums whole instructions until it covers the
  5 bytes of the JMP and gives up on any opcode outside the prologue
  vocabulary — copying half a relative jump would be worse than not hooking.
- A code detour applies to **every** device in the process, including ones
  other mods create. That is why `EndSceneHook` only draws when the device is
  the one captured in `CreateDevice`.

The vtable hook stays in the code as a fallback, for when the prologue cannot
be copied.

## Speed, RPM and throttle — the car mirror

Pops looked for telemetry in the physics object and only found the gearbox
snapshot, which freezes between shifts. The good path is the car **mirror**:

```
player   = *(DWORD*)0x8900AC        (direct, not an array)
mirror   = *(player + 0x04)
  +0x42C  speed (m/s)
  +0x438  throttle (0 / 0.7 / 1.0 on keyboard)
  +0x440  RPM (800 at idle up to ~7000)
  +0x4E8  distance travelled
```

Great to READ, **useless to write**: `+0x440` is the output of a low-pass
filter (written at `0x5A5A51`, source in global `0x7FBED8`), so writing there
holds the dial and the engine sound while physics ignores it. The real
authority is the physics object, captured by the hook at `0x5A5540`.

The gearbox snapshot (`mirror+0x34 -> +0x18`, gear at `+0x34`) is still the
only source of gear, and `speed.game.telemetry()` merges the two. **The count
is off by one: 1 is neutral**, 2 is first gear, 0 is reverse. The SDK reports
the raw number; whoever displays it applies the shift.

## Turbo, and the vehicle-hook trap

Boost lives in the engine object, reached through the vehicle physics — which
is not stored in any global. The way to get it is to wait for the game to call
`0x5A5540` (`__thiscall`, the vehicle in `ecx`):

```
physics + 0x48   engine object          physics + 0x58   the car mirror
engine  + 0x9C   current pressure       engine  + 0xA4   car ceiling (0 = naturally aspirated)
engine  + 0xAC   vacuum floor
```

**`0x5A5540` runs for every vehicle in the world**, traffic and opponents
included. Storing `ecx` from any call made the object flip between cars several
times per frame, and the boost needle in the HUD turned into noise. The filter
is the mirror: only the vehicle whose `physics+0x58` matches the player's
mirror counts. The read re-checks, because the stored pointer goes stale when
the player changes car.

Pops hooks `0x5A5540` too. Since an `E9` cannot be copied into a trampoline (a
copied rel32 points at the wrong place), FRSModLoader installs in front and
jumps to the previous hook — both run, in installation order.

## Hiding the stock HUD — the frontend package switch

The in-race HUD is a frontend package, just like the menu screens:
`HUD_SingleRace.fng`, `HUD_Drag.fng`, `HUD_Drift.fng`, `HUD_Short_Track.fng`,
`2PHudTop/Bot.fng`. They live in the same 28-byte table format as the screens,
at `0x7F8228` and neighbours (the menu screen table is at `0x7F9138`).

The engine keeps a registry of those packages at `0x8637B0` and decides whether
one counts by testing a word at `+0x1E`:

| Address | What it does |
|---|---|
| `0x51CE80` | `__cdecl (const char* name, short value)` -> writes `[entry+0x1E] = value` |
| `0x51BB70` | `__thiscall` lookup of the entry by name in registry `0x8637B0` |
| `0x51C6D0` | returns `[entry+0x1E] != 0` (the test the engine performs) |

So hiding the HUD is writing 0, and restoring it is writing 1 — the same switch
the game itself uses, with no code patching. A mod does it from JavaScript, on
a key.

Two things learned in practice:

- **It must be reapplied.** The package only accepts the switch once it exists,
  and it is born slightly after the gameflow turns 6. Rewrite it twice per
  second while the race runs: six word writes are cheaper than mapping
  every moment the game rebuilds the package.
- **The rear-view mirror is not part of it.** The mirror strip at the top of
  the screen keeps showing — it is not an FEng package. It is probably
  `HUD_FEATURE_RVM` from the feature table (below), drawn by another path.

### The HUD_FEATURE_* table, and why it went unused

There is a table of 64 names at `0x7F6728` (`HUD_FEATURE_TRACK_MAP`,
`HUD_FEATURE_GAUGES_1`, `HUD_FEATURE_NITROUS`, `HUD_FEATURE_RVM`...) that looks
like a 64-bit mask giving fine-grained HUD control. **Nothing in the code
references that table** — they are debug names left over in the build. The mask
itself probably exists, but finding what tests it is a separate job; the
package switch solved the case for far less.

## Spawning a car — the creation path

Mapped statically, from the binary. The entry point the game itself uses is

```
0x606480  __fastcall CreateCar(transform /*ecx*/, slot /*edx*/,
                               int driverType, void* spawnNode)
```

and it is called once per racer from `0x6065B0`, the function that puts the
grid on the track. That one is worth reading as documentation: it asks
`0x5EB5D0(6, &out)` for six starting-grid transforms (0x20 bytes each), walks
the racer array and, for each entry, calls `CreateCar` with either the grid
transform or the entry's own position (`entry+0x920`, used when the byte at
`entry+0x91C` is set).

**The racer array** is the list of cars the world is supposed to have:

| Address | What |
|---|---|
| `[0x89E810]` | base of the array, entries of **0x940 bytes** |
| `0x89E814` | how many |
| `entry+0x04` | 1 the player, 2 an opponent |
| `entry+0x10` | the **car spec** (see below) |
| `entry+0x8F8` | streaming handle of the model |
| `entry+0x91C` | "has its own spawn position" |
| `entry+0x920` | that position, as the 0x20-byte transform |

The same 0x940-ish shape shows up in the traffic manager (`0x412950`) and in
the frontend's car display: everything that needs a car on screen holds one of
these.

**The car spec** (`entry+0x10`, ~0x900 bytes, copied by `0x413440`) starts with
the model INDEX into the table at `[0x8A1CCC]` (46 entries of 0x890), then
colour bytes at +4..+7, then the customization from +0x10 on. This is what
picking a model and dressing the car will write into.

**What `CreateCar` does**, and what a mod can do instead, since `speed.call`
cannot pass `edx`:

```
0x639CE0  thiscall(spec, flag, 1, 1, 1)     prepares the render side
0x575620  cdecl new(0x9E0)                  the car object
0x604260  thiscall Car::Car(slot, &pos, word, heading)   ret 0x10
0x5EBA30  thiscall Car::SetAIType(0..5)     1 racer, 4 traffic; 0 is what the
                                            constructor already installed
0x5F46A0  thiscall Car::SetDriverType(t)    the switch at the end of every
                                            autopilot call (0x5FAE20/0x5FAF10)
[0x890080]                                  the world's car list: +0 head,
                                            +4 tail, and the car's own +0/+4
                                            are next/prev
```

The constructor takes the position as a plain vec3 (it reads +0, +4, +8 only)
and the heading as a float: zero takes a different branch (`0x5DA3D0`), any
other value goes through `0x5DA410` and lands as a direction vector in
`car+0x60`. The position ends up in `car+0x510`, and `car+0x14` points back at
the slot.

One trap found before writing any code: `SetDriverType` indexes two arrays,
`0x89CD50` and `0x89CDA8`, by `word[car+8]` — and the constructor leaves that
word at **zero**, the same index the player's car uses. Calling it on a spawned
car zeroes the player's entry and takes his controls away. A parked car does
not need it: the constructor already gives the car the null driver
(vtable `0x7A5BB0`).

`mods/spawn-car` does this: it clones the player's slot, writes another model
index into the spec, waits for the model to stream in, and builds the car at a
saved coordinate.

**Choosing the model** is writing the index of the table at `[0x8A1CCC]` into
the first dword of the spec — and then getting that model off the disk, which
is what the game does before every race (`0x57F6D1`) and for every traffic car
(`0x412B97`):

```
0x8A3288              the car loader
0x63BC70  thiscall    RequestLoad(spec*, a, b) -> handle, kept at slot+0x8F8
0x63BCD0  thiscall    Start(callback, param)
0x63AE00  thiscall    Release(handle)
```

`Start` refuses to do anything while `loader+0x14` is not zero — it is the
first test in the function. That field is the mod's "still loading": no native
callback stub needed, just poll it from the frame hook and build the car when
it clears.

### Asking for a model that is not loaded — what actually happens

It does not work yet, and the failure is worth writing down because it looked
like three separate bugs and was one.

Requesting a model the world does not have swaps the TEXTURES of the cars
already on screen, leaves their geometry alone, and the spawned car comes out
wearing the old model. Then, on the first collision, the game dies at
`0x593A5E`:

```
00593A5B  mov eax, dword ptr [ecx + 0x68]
00593A5E  mov edx, dword ptr [eax + 0x158]     <- eax is null
```

`ecx` there is the car's simulation object, `car+0x3C`, built by `0x5AEC20`;
`+0x68` is where it hangs **the model's data**, and `0x5AEC20` only zeroes it.
So: the model never really loaded, the constructor did not complain, and the
car walked around for a while with no data behind it until something asked.

Two things fall out of that:

- The constructor **already calls `Car::SetSpec` (0x601110)** at `0x6046AA`,
  with its own slot's spec. Applying the spec is not a step a mod has to do
  after the fact — which also means the model the car ends up with is readable
  at `car+0x0C`, the model-table entry SetSpec resolved. Logging what was asked
  for next to what was born is what turned this from guesswork into a fact.
- `car+0x3C -> +0x68` is **not** the model's data, which is what the first
  guard assumed. An x-ray printed in-game settled it: every traffic car in the
  world drives around with that field null, and only the player has it — plus
  one opponent, at the exact moment it picked up AI type 3. It is the
  controller object that `0x5EBA30` builds. The honest test for a spawned car
  is comparing the model that was asked for with `car+0x0C`, the model-table
  entry SetSpec actually resolved; a car wearing the neighbour's geometry is
  thrown away without ever entering the world list.

### The slot array — cars live in a fixed table, not in a mod's buffer

`0x5ED5F0`, the container's constructor, writes `[edx+0x70] = 0x890160`: the
slots are a **static array** that is born with the game, 0x940 bytes each,
with the count at `+0x74` (`0x89E814`). And a car's index — the word at
`car+8` — is its position in that table: the traffic SUV at index 7 lives at
`0x890160 + 7*0x940`, which the x-ray confirmed address by address.

That is the thing the mod had wrong from the start. A car does not carry a
spec the mod can hand it; it belongs to a slot in this table, and the loader
finds the model's owner by walking the same table (`0x63B360`). A clone of
the player's slot sitting in memory the mod allocated is not in the table, so
nothing claims the model that gets loaded — the geometry goes nowhere, the
textures land on top of whoever is already there, and a car built from it has
`word[car+8] = 0`, which is the player's own index.

The free-roam x-ray also shows how much room there is: the player at index 0,
opponents at 1 to 3, and the traffic filling 4 to 13 — all of it streamed in
while driving, models the race never asked for. Traffic is the proof that
loading an arbitrary model at runtime works; it just does it through a slot.

**Occupying a slot** is `0x601600`, `__thiscall AddCarSlot(prototype)` on the
container: it copies the prototype (`0x5FC130`) into `base + count*0x940` and
increments the count, refusing above `0x16` — **22 slots, and no more**. The
copy takes the four header bytes, the type at `+4`, the spec, and the tail
from `+0x900`; it deliberately leaves `+0x8F8`, the streaming handle, to
whatever the destination slot had, which in a free slot is zero.

A free slot is easy to recognise in the table: type 0 and a spec whose model
index is `-1`, the same `-1` that makes `0x639CE0` return immediately. A
free-roam session uses 14 of the 22, so there are eight to spawn into.

The other fields the x-ray made legible:

| | |
|---|---|
| `slot+0x01` | an id byte — 0 the player, 50-59 traffic, 100+ free-roam racers; the container's `find` (`0x5ED920`) searches by it |
| `slot+0x03` | starting-grid position, 0 for traffic |
| `slot+0x04` | 1 player, 2 racer, 3 traffic |
| `slot+0x8F8` | the load handle, **shared between slots using the same model** — two TAXI02 slots both point at handle 8 |
| `slot+0x908` | the model index again |

### The model index lives in the spec twice

Occupying a real slot fixed the ownership, and the car still came out wearing
the player's body. `car+0x0C` said TIBURON while the geometry on screen was
the player's PEUGOT, with the player's parts fighting over it — which is what
made the textures land on the wrong car.

The reason is that the spec keeps the model in more than one place, and
`0x610270` — `__thiscall CarSpec::Init(model, ?, colour, colour)`, `ret 0x10`
— is the function that knows all of them:

```
[spec+0x00] = model            the one a mod finds first, and the only one it
                               is tempting to write
zero 0xAA dwords from +0x590   the parts
0x01010101 across +0x838
0x5A7890(spec+0x10, model)     writes the model AGAIN, at spec+0x10 and at
                               spec+0x384
```

`car+0x0C` is resolved from `spec+0x00`, so the car reports the model that was
asked for; the geometry comes from the other copy, which still said PEUGOT.
Writing one dword was never going to be enough — and the parts inherited from
the player's car were being applied to a body that has no such parts.

### A car with no parts has no body

`Init` leaves the spec clean, and too clean: it zeroes the parts (0xAA dwords
from `spec+0x590`), and in Underground 2 **the body is a part**. A car spawned
from a freshly-initialised spec arrives as a shadow and nothing else — the
shadow is found because it is a separate resource, `"%s_SHADOWIG"` looked up
by hash (`0x4901D0`), which also proves the model itself did load. Meanwhile
the renderer keeps trying to resolve a body that is not there, every frame,
and the game drops to 9 fps. Both symptoms, one cause.

The full recipe is the one the garage uses to build a new car (`0x503A80`):

```
0x610270  CarSpec::Init(model, 1, 0, 0)
0x61C280  CarSpec::SetSkin(n)        builds "DUMMY_SKIN%d"
0x637040  CarSpec::DefaultParts(n)   for each part index, asks 0x61BC80 what
                                     the default part for THIS model is and
                                     writes it to spec+0x590 + index*4
```

`0x6102F0` is the matching getter (`part(i) = [spec + i*4 + 0x590]`), useful
for checking from a mod that the body actually went in.

### Every car needs its own texture number

`0x61C280` builds `"DUMMY_SKIN%d"`, `"DUMMY_WHEEL%d"` and `"DUMMY_SPINNER%d"`,
hashes them and stores the results in `spec+0x584` and `+0x588`. The number is
the car's **texture space**: two cars sharing it share the textures, and the
one that loads last wins. The garage passes `index+1`, so the player, at slot
0, is `DUMMY_SKIN1` — and a mod that passes a literal 1 dresses the player in
the spawned car's paint. The number has to come from the car's own slot.

The skin hash is worth logging next to the player's when spawning: two equal
hashes are the bug, visible before anything reaches the screen.

### The collision crash: `sim+0x0C`, not `sim+0x68`

The crash at `0x593A5E` survived every change, and the reason it resisted is
that the null field belongs to a different object than the one being read:

```
005AF38C  mov ecx, [esi + 0x0c]      ; the sim's COLLISION object
005AF3A4  call dword ptr [edx + 0x60]
00593A5B  mov eax, dword ptr [ecx + 0x68]
00593A5E  mov edx, dword ptr [eax + 0x158]    <- eax null
```

`sim+0x0C` is the model's collision geometry, looked up by `0x5A18C0` in a
global list at `0x86B598`, keyed by what `0x61C320` builds out of the model
name and **parts 5 and 6**. When that lookup fails, the sim's constructor
(`0x5AED58`) makes a default box of **0x40 bytes** — six floats, a bounding
box — and sets `sim+0x10 = 1`.

The real object has fields at +0x68, +0x158, +0x170, +0x1B0. The default box
is 0x40 bytes long. Reading +0x68 from it is reading past the end, and the
first collision does exactly that.

`sim+0x10` is therefore a **reliable, game-provided verdict** on a spawned
car: 1 means "this car will take the game down when something touches it".
The mod checks it right after the constructor and refuses to put such a car
in the world.

Three candidates at the same offset, told apart by what they hold, measured
in-game across every car in the world:

| | player | traffic | opponent | spawned |
|---|---|---|---|---|
| `col+0x68` | `0x8f0006` | `0x60005` | `0x3e0005` | `0xc5000a` |
| `ctrl+0x68` | float | float | float | garbage |
| `sim+0x68` | pointer | **0** | pointer | **0** |

`col+0x68` and `ctrl+0x68` are two packed words and a float: if `0x593A5E`
dereferenced either, touching any car would kill the game. `sim+0x68` — the
object built by `0x5AEC20` and hung off `car+0x3C` — is the only one that is
a pointer on the cars that work and null on the spawned one.

Traffic has it null too and never crashes, which says the method is only
reached for cars the game treats as racers.

`sim+0x68` is a **RigidBody**: the pool is named `RigidBodySlotPool`
(`0x598F10`), its objects are 0x210 bytes — exactly the gap between two
opponents' values — and what `0x593A40` reads from it, `+0x158` and `+0x1B0`,
is mass and inertia. Traffic has no rigid body at all; it runs on simplified
physics, which is why touching it never crashes.

Two things were ruled out by measurement rather than reading:

- **Neither the driver type nor the AI type produces one.** All eight
  combinations of `SetDriverType(0..5)` and `SetAIType(0/1/3/4)` leave the
  field null, on two different models. `0x5B6FC0` and `0x5B5CC0` do take a
  rigid body from the pool, but they hang it inside the driver, not on the sim.
- The only function in the binary that stores a pool rigid body into a `+0x68`
  is `0x5B1540`, reached from the constructor `0x5B3240` — of an object that
  comes from a **different pool** (`0x86B374`). So the rigid body is not
  attached to a sim after the fact: it is born with a sim of another class,
  and `0x5AEC20` builds the simplified one.

### The gate at `car+0x5C0`

The rigid body is hung on the sim by `0x5938C0`, which `SetDriverType` calls
at the end:

```
if ([sim+0x5C] == 0) return;                    leaves +0x68 null
if ([car+0x28] is not 1 and not 3) return;      leaves +0x68 null
[sim+0x68] = [ [car+0x34] + 0x20 ]              from inside the DRIVER
```

Only cases 1 and 3 of the switch write `[sim+0x5C] = 1`, and the type-1 driver
(`0x5B5CC0`) is one of the functions that take a RigidBody from the pool. So
asking for driver type 1 should be all it takes — and asking was not enough,
because `SetDriverType` opens with:

```
005F471E  cmp byte ptr [esi+0x5C0], bl   ; this byte being 0...
005F472A  cmp edi, 4                     ; ...and a requested type other than 0 or 4
005F473A  mov [esp+0x24], eax            ; silently rewrites the type to 0 or 4
```

The constructor leaves `car+0x5C0` at zero, so every request for type 1 or 3
was quietly downgraded to 4 — which then fails the filter in `0x5938C0`. The
whole eight-combination sweep came back null because the game never tried what
the mod asked for. The log line that cracked it was one number: `car+0x28=4`
after asking for 1.

`0x5F6CD0` is what sets the byte: `thiscall(car)`, deriving it from the car's
position (`car+0x60`) through `0x5E68D0` — it means "this car is in an active
part of the world". Calling it after the constructor lets the game decide;
writing 1 by hand is the fallback.

With the gate open, a spawned car gets its rigid body and survives being hit.
Step two is done: pick a model, spawn it standing at a saved coordinate, walk
into it.

Two details that only show up once several cars exist at the same time:

- The slot scan that picks a free `DUMMY_SKIN` filtered on slot types 1 and 2
  and skipped **3** — the very type this mod's slots use. Each spawned car
  therefore could not see the previous one's skin and asked for the same
  number, so four cars shared one texture.
- `Init`'s last two arguments are the paint colours (`spec+5`, `spec+6`), and
  passing 0 gives every car the same factory colour. Varying them per slot is
  enough until step three makes it a choice.

### How many cars fit

Two limits, and the one that bites is not the obvious one:

- **22 slots**, hard, from the static array at `0x890160`. A free-roam session
  already uses 14 (player, 3 racers, 10 traffic), so 8 are left.
- **Six textures.** `DUMMY_SKIN1` to `6` exist and nothing beyond — six is the
  size of a race grid. Measured, not guessed: with the player on 1 and racers
  on 2-4, the third spawned car asked for 7 and came out invisible, the same
  symptom as the earlier `DUMMY_SKIN15`. A number with no resource behind it
  is a body with no texture.

Counted in the game's own files rather than guessed: hashing `DUMMY_SKIN<n>`
with the real function (`0x43DB50`: `h = 0xFFFFFFFF; h = h*0x21 + c`, which
reproduces the two values read out of memory) and searching the `.bin` files,
`DUMMY_SKIN1` to `6` appear in `GlobalMemoryFile.bin` and `CARS\TEXTURES.BIN`,
and 7 onwards appear nowhere. Six is not a limit in code that can be patched
out — it is the absence of an asset.

Going without one does not help either: traffic lives with a zeroed skin hash
and shows up fine, but a **race** model with no skin comes out transparent,
which was measured — the third spawned car was invisible both when it asked
for `DUMMY_SKIN7` and when it asked for nothing.

The field that decides is `CarTypeInfo+0x88C`, tested at `0x61C2B3`: zero
means the model does not use a custom skin at all. So:

- **race models** cost one of the six, and a free roam (player + 3 racers)
  leaves two;
- **traffic models** cost nothing and are limited only by slots.

Past six, sharing a number with another car *the mod spawned* beats vanishing.

### Step three: parts and vinyls

An installed part is a **pointer** in the spec, at `+0x590 + slot*4` — the
same place `SetStockParts` fills, and what the Unlimiter writes as
`RideInfo[CarSlotID + 356]` (356 dwords is 0x590, which confirms the offset
found here by hand). Changing a part is writing that pointer and telling the
car:

| | |
|---|---|
| `0x8A2B68` | the car part database |
| `0x61BA30` | `thiscall FirstCarPart(carType, slot, hash, level)` |
| `0x61BA50` | `thiscall NextCarPart(previous, carType, slot, hash, level)` — hash 0 and level -1 mean "any" |
| `0x610000` | `thiscall CarPart::GetName() -> char*` |
| `0x61BCD0` | `thiscall RideInfo::UpdatePartsEnabled()` — without it the part is in the spec and the body does not know |
| `0x802DE8` | slot names: pairs of `{int id, char* name}` until a null pointer |

Read out of the exe, the slots run from `BASE` (0) to `VINYL_COLOUR3_3`, with
**`VINYL_LAYER0..3` at 64-67** and the paints at 63 and 68-76. So vinyls and
paint are not a separate system: they are slots like any other, and the same
write-then-update does all of it.

### Challenging a spawned car to an outrun

The game already has this: the **RandomEncounterManager**, whose states are
spelled out in the exe at `0x7E9B78` — `NONE`, `POLL_CAREER`,
`LOADING_OPPONENTS`, `TRYING_TO_ENGAGE`, `RACING`, `DEACTIVATE_OPPONENT`.

| | |
|---|---|
| `[0x890118]` | the encounter object; `0x60A282` reads it and runs its update (`0x6076D0`) |
| `obj+0xC30` | a flag — the update sees it set, calls `0x607180`, and clears it |
| `0x607180` | builds the encounter: writes 7 into `[0x89E7B0]` (the mode) and gathers participants |
| `obj+0xC40` | the RandomEncounterManager itself |

Setting the flag is better than calling `0x607180` from a mod: the game runs
it at its own point in the frame, which is where it expects to be.

The detail that decides whether a spawned car can be challenged at all:
`0x607180` collects opponents from **group 2** of the world car list — the
cars whose *slot* is type 2, racer — using the grouping `0x5F0D20` builds. A
car spawned as traffic (type 3) is invisible to it. So the challenge promotes
the car to racer first, and only then arms the flag.
**The slot array, on the other hand, can be grown.** Nothing reaches
`0x890160` directly — the code goes through `[container+0x70]`, written once
in the constructor at `0x5ED69C` — so a bigger array can be allocated,
copied into, and pointed at, with the ceiling (`cmp [esi+0x74], 0x16` at
`0x60163D`) patched to match. The Unlimiter does exactly this to the traffic
manager, relocating it to `0x1230 + 0x920 * 64` bytes of its own.

**A mistake worth not repeating:** an attempt to sweep the combinations
automatically — build a car, check the field, discard and try the next —
corrupted the heap and made the game die at spawn, jumping to 0xBCA3D70A. The
car constructor *registers* the car in the game's lists (CarRenderInfo in the
render list, the sim in its own), and a discarded car does not leave them.
Seven half-built cars being updated every frame is not "leaking 0x9E0 bytes",
which is how the idea was costed. Build one car, once.

### Names, from NFSU2Unlimiter

nlgxzef's Unlimiter (github.com/nlgxzef/NFSU2Unlimiter) is a customization
mod, so it does not touch the physics side, but it names a lot of what this
hunt had to find by hand — and it confirms the map:

| Address | Its name |
|---|---|
| `0x8A1CCC` | `CarTypeInfoArray` (the model table) |
| `0x89E7A0` | `TheRaceParameters` (the slot container) |
| `0x637040` | `RideInfo::SetStockParts` (the factory parts) |
| `0x61BC80` | `FindPartWithLevel(RideInfo*, CarSlotID, level)` |
| `0x6102F0` | `RideInfo_GetPart(RideInfo*, CarSlotID)` |
| `0x631D20` | `RideInfo_SetPart` |
| `0x61BCD0` | `RideInfo_UpdatePartsEnabled` |
| `0x638190` | `CarRenderInfo` constructor `(RideInfo*, Car*)` |
| `0x412950` | `StreamingTrafficCarManager::Update` |
| `0x802DE8` | `CarSlotIDNames` — the part slots, by name |
| `0x4A7890` | `GetRideInfo(which_car)` |

So the 0x940 slot is a **RideInfo**, and the parts written at `spec+0x590 +
i*4` are indexed by **CarSlotID**, whose names are in a table in the exe.
Step three is `RideInfo_SetPart` plus that table, which is a far shorter road
than step two was.

### The loader, and the signal that actually means "loaded"

`0x63B160` is the pump, and it is a state machine driven by asynchronous I/O,
not a function that loads anything before it returns. It walks the request
list, hands a file to `0x4941C0` with `0x610A10` as the completion callback,
sets `loader+0x14 = 1` and leaves. The callback zeroes that field and calls
the pump again, which picks up where it left off. Traffic cars stream in this
way while you drive, so it works at runtime — nothing about it needs a loading
screen.

Which makes `loader+0x14` a trap: it is "a read is in flight", and it goes
back to zero **between** reads. Waiting on it means building the car in a gap
several files before the model exists, which is exactly how the first attempt
got a car with no data and a crash on contact.

The truth is in the request:

| | |
|---|---|
| `loader+0x58` | sentinel of the request list (circular, `+0` next, `+4` prev) |
| `req+0x0C` | **state: 1 loading, 2 ready** (`0x63B41E` writes the 2) |
| `req+0x20` | the request's own copy of the spec — `[req+0x20]` is the model index |
| `req+0x910` | the model-table entry |
| `req+0x123C` | the number `RequestLoad` hands back |

`RequestLoad` returns the number, not the node, so finding the request means
walking the list from the sentinel and matching `+0x123C`. Then wait for
`+0x0C` to reach 2, and only then build the car.

One detail worth keeping in mind for later: at the end of a request the pump
searches the racer array for a slot using that same model (`0x63B360`), and
what it does next depends on the gameflow — in the frontend (state 3) and at
boot it takes another path entirely, through `0x61C600`. So "load a car" is
not one behaviour: the engine already distinguishes a car for a race from a
car for a menu.

## Challenging a spawned car to an outrun

A spawned car can be raced. The recipe is short, but only two lines of it are
the ones that matter, and both were found by comparing a real outrun against
ours in the same log rather than by reading the disassembly.

```
slot+0x04 = 2                                   the slot becomes a racer's
car+0x0A  = <free racer id>                     the game's own are 100..102
SetAiType(car, 3); SetDriverType(car, 3)
0x5F0D20(world)                                 regroup: puts it in the racer bucket
player+0x250 = car                              <- THE RIVAL
manager+0xC30 = 1                               arm; the update assembles it
```

**`player+0x250` is the rival's car.** Without it there is no outrun, only a
race everybody has already lost. `0x607180` walks the participants and marks
every one of them "lost" (`word[part+0x10] = 7`), skipping exactly two: the
player, by `slot+4 == 1` at `0x60736C`, and the car at `player+0x250`, at
`0x607376`. Those two survive at state 0 — and state 0 is the only state the
loop at `0x607980` promotes to 3, "racing". A null pointer there means the
challenged car is never promoted, so the AI never drives it, the HUD has no
target for the arrow (it falls on the player), the distance readout has no
reference, and `0x5EE250` eventually has the car removed.

The participant object lives at `car+0x1C`. `word[part+0x10]` is its state in
the machine at `0x606CB0` = `SetParticipantState(manager, index, ..., state)`,
which takes the state as its fourth argument and dispatches through a table for
4..7. 0 is racing, 3 is in the race, 6 won, 7 lost. `part+0x0A` is the finishing
position — `0x6078CD` picks 6 or 7 from it. It is not `car+0x0A`, which is the
racer id; confusing the two cost several rounds.

Two other things end an outrun before it starts, both of which are symptoms of
the same missing rival and both of which disappeared once it was set:

`0x5EA530` stamps `player+0x50` with the clock the moment the player is
considered finished, and `0x60B5BD` ends the race when
`[0x7FB720] > player+0x50 + player+0x54`. With `+0x54` at zero the deadline
expires on the next frame. `0x607180` calls it from inside its bookkeeping
loop — the loop that should not be running at all in a healthy outrun.

`0x42C0C0` asks `0x5EE250` every frame whether a participant is still racing,
and `0x5EE250` answers from that one field: a participant not at state 0, while
a race mode is set, gets `SetDriverType(car, 1)` — it stops being a racer, goes
translucent and is collected.

### Six participants, and not one more

The participants live at `manager+0x30` with a `0x1F0` stride, and the array of
pointers to them starts at `manager+0xBD0`:

    (0xBD0 - 0x30) / 0x1F0 = 6, exactly

Six fit. With the three free-roam racers, the player and three cars of ours all
promoted to racer slots, `0x607180` constructs the seventh on top of the pointer
array, and `0x5FC2A0` then calls `0x5EDE40` over fields that are now something
else — it read `0x3F9C28F6`, a float, as a pointer. So: promote only the car
being challenged and demote the previous one, which is also the game's own model
(an outrun has a rival, not a roster), plus a hard check before arming.

That diagnosis took one run because the plugin carries a vectored handler that
logs, at the moment of the fault, the address, the registers, the game return
addresses still on the stack and how many cars the mod has where. The Event
Viewer only gives the offset, and the log simply stops.

### What the three flags are not

`0x89CF69`, `0x8669F4` and `0x866A14` are read all over this code and look like
escapes from the race ending. They are the opposite: they select the **race
event** path (circuit, sprint, drag, with its loading screen). A world outrun is
the fall-through with all three at zero. Setting `0x89CF69` sends `0x5FC510`
down a path that indexes `0x89CF50` by `byte[0x89D020]`, structures that do not
exist in free roam, and the game faults at `0x5FC577`. Nothing *writes* that
byte, which is not the same as nothing depending on it.

### Method

Four rounds of "read the branch, form a hypothesis, ask for a game run" produced
four wrong hypotheses; `0x6076D0` is too large a state machine to deduce a
branch at a time. What worked, every time:

- **A hardware breakpoint** on the field (DR0 + DR7, a vectored handler logging
  EIP). It named `0x5EA54A`, `0x5F4709` and `0x607E52` on the first try each.
  It must ignore writes coming from the plugin's own module, or it catches the
  diagnostic instead of the culprit.
- **Intercepting a setter** and recording `_ReturnAddress()`, when the field is
  written through a function with dozens of call sites.
- **Capturing the game doing it right.** The plugin notices `0x89E7B0` going to
  7 with no challenge of ours in flight, and dumps the same fields it dumps for
  ours. `player+0x250` appeared in the first diff, after several rounds of
  failing to find it by reading.

## Open

**The rear-view mirror.** Still on screen after the HUD is hidden. Likely path:
the `HUD_FEATURE_RVM` mask, or the function that draws the mirror.

**CEF popups** (`PET_POPUP`): `<select>` and friends are not composited yet.

Diagnostics that stay in the binary, useful when something stops showing up:

- `[gfx] EndScene alive: N frames` every 5 s — if it stops, we lost the detour.
- `[gfx] overlay: visible=1 1280x800 texture=1280x800 content=1` — where
  `OnFrame` is bailing out, if it is.
- `[UI] TestPattern = 1` in the ini swaps CEF's content for a checkerboard: it
  separates a compositing problem from a page problem.

## Details that cost time

**F10 never reaches a mod** — or did not, until now. Windows delivers F10, and
anything held with Alt, as `WM_SYSKEYDOWN`, because F10 is the key that opens
the window menu. `InputRouter` only listened to `WM_KEYDOWN`, so a mod that
bound F10 waited for a message that was never coming, and the silence looked
exactly like a broken mod. The router now treats both as a key press and
swallows the F10 afterwards, so `DefWindowProc` does not open the window menu
and drop the game out of focus mid-race.


**`libcef.dll` failing with error 126.** It lives in `scripts\FRSModLoader\`,
away from `SPEED2.EXE`. Windows resolves a DLL's dependencies from the
*executable's* directory, so `chrome_elf.dll` sitting next to it was not found.
Fixed with `LoadLibraryEx(..., LOAD_WITH_ALTERED_SEARCH_PATH)`, which makes the
DLL's own folder the search root. `libcef.dll` is delay-loaded (`/DELAYLOAD` in
CMakeLists) precisely so that manual load happens before the first call.

**`static` in a header.** `Config::g_ownDir` was a `static char[]` inside the
namespace in a `.h`: every `.cpp` got its own copy and only `dllmain.cpp`'s was
filled in. `CefHost.cpp` was building paths from an empty string
(`\FRSModLoader\libcef.dll`). It is `inline` now (C++17). Worth checking `Pops`,
which has the same pattern — it just never shows there because Config is used
from a single file.

**`CRITICAL_SECTION` before CEF.** The game draws long before Chromium starts,
and `Overlay` was already calling `LockFrame` on the first frame — entering an
uninitialised critical section, which killed the game at 0.17 s. The lock is
now born with the DLL (a global object), not in `Init`.

**CEF initialises on the first frame, not in `DllMain`.** Starting threads and
loading DLLs under the loader lock is asking for a deadlock. `DllMain` only
checks the executable version and plants the hooks; CEF, QuickJS and the mods
come up a second later, from the main loop.

**The game runs with `scripts\` as its working directory.** The log lands
there, not in the game root — same behaviour as Pops.

**CEF 152 no longer has `CefEnableHighDPISupport`**, and the *minimal*
distribution ships no `cef_sandbox.lib` (hence `no_sandbox = true` and
`--no-sandbox`).

**`/MT` everywhere.** CEF requires the static runtime; QuickJS defaults to
`/MD`. Without a global `CMAKE_MSVC_RUNTIME_LIBRARY`, the link breaks on
`__imp__acosh` and friends.

## Next steps

1. The rear-view mirror, the last piece of stock HUD still on screen.
2. CEF popups (`PET_POPUP`): `<select>` and friends are not composited yet.
3. A custom scheme (`frsmodloader://`) instead of `file://` plus
   `--allow-file-access-from-files`.
4. `speed.hook.*`: today you can patch bytes and redirect a `call`, but there
   is no "run my JS when the game reaches this address".
5. Reloading a mod without restarting the game (the `JSContext` is already per
   mod, so it is a matter of dropping and rebuilding it).

## Engine, turbo and the ignition cut

All confirmed at run time, not read off the disassembly. Brought over from the
Pops ASI that predates this platform.

### The mirror is not the car

`player+0x04` is a telemetry MIRROR, filled by 0x5A5540 copying physics into
`[esi+0x58]`. Read it freely; writing to it moves nothing. Its RPM (+0x440) is
the output of a low-pass filter (written at 0x5A5A51, fed from the global
0x7FBED8), so it also lags.

The physics object is the `this` of 0x5A5540. Capture it by filtering on the
mirror (see Vehicle.h) — 0x5A5540 runs for every vehicle in the world, and
keeping the last `this` gives you an opponent's car. That mistake cost a whole
investigation here: an effects array that "never changed" was simply someone
else's.

### Engine fields (physics+0x48)

    +0x14  engine speed, in RAD/S   (rpm = rad/s / 0.1047197551)
    +0x28  idle        (800 or 850, per car)
    +0x2C  rev limit   (7000 or 7500, per car)
    +0x78  multiplier applied to the throttle
    +0x80  input throttle (0 / 0.7 / 1.0 on a keyboard)
    +0x9C  boost pressure, negative under vacuum
    +0xA4  the instantaneous TARGET pressure from the engine map. NOT the car's
           ceiling: it falls to ~0.09 while the throttle is cut. Reading it as a
           maximum is a trap.
    +0xAC  vacuum floor

Naturally aspirated cars keep +0x9C near zero and +0xA4 at zero.

### Order inside a frame

    input writes +0x80  ->  physics consumes  ->  0x5A5540  ->  mods run

Anything written from a frame callback arrives after the physics has already
read it. Three separate attempts failed on this, each looking like a different
bug:

* pinning the RPM made the speed climb without limit — RPM is state, and fixing
  a state variable does not cut power, it breaks the integrator that derives
  everything from it;
* zeroing the throttle from the loop did nothing — the input rewrites it first;
* writing the boost from outside produced a sawtooth — it is the output of an
  integrator that runs at the start of the frame.

The rule that came out of it: inputs can be written, but only from inside the
pass; state must not be written at all.

### The ignition cut the game already has

At 0x5A1629, right after the input writes the throttle:

    fld   [esi+0x5C]        ; timestamp of when the cut began
    fcomp [0x7D7D3C]        ; 0.0 — anything else means "cut"
    mov   [ecx+0x80], ebx   ; throttle = 0
    fild  [0x86518C]        ; the game's clock, an int counter
    fmul  [0x784268]        ; * 0.00025 -> seconds
    fsub  [esi+0x5C]
    fcomp [0x7A18C8]        ; 0.45s, then it clears the stamp itself

So a launch limiter is: stamp `*(int*)0x86518C * 0.00025` to cut, and write 0 to
stop cutting. The 0.45s is tuned for a gear change, where dropping the revs is
the point; for anything else, clear it early or the engine falls away.

A gear change does NOT go through this timer — that cut is another mechanism.
To detect a shift, use the gearbox snapshot (`*(mirror+0x34) + 0x18`, gear at
+0x34), which the game writes only on a shift and leaves frozen in between.

### Turbo

The model is one function, 0x5A0F30, with a single call site at 0x5AA7F0
(__thiscall, one stack argument, `ret 4`). Its integrator is at 0x5A1094.

Anti-lag, done right: wrap the call and let the function run seeing the throttle
at 1.0, then put the real value back. The game's own model then fills the turbo
with its own curve. Holding the pressure by writing it never works, for the
reason above. The same trick held boost across gear changes, which is worth more
over a lap than any launch trick.

### Car effects (CARFX) — mapped, not solved

The effect table is at 0x8027C0, 8 bytes per entry, and 0x430C90 (name -> id)
confirms the numbering: 10 NITRO, 11 EXHAUST_SMOKE, 12 EXHAUST_BLOWOFF,
13 NOS_BLOWOFF.

The player's effects object is `*(mirror+0x554)` (confirmed both ways: the
mirror points at it, and it points back at +0x00). It holds two arrays of 30
circular list heads, at +0x458 and +0x548.

What does NOT work, so nobody repeats it: those lists never change when an
effect plays (~1000 samples at 20 Hz, with nitrous, drifting and a crash), and
that object is not the container of the effect rules either. The rules are
loaded by 0x432CA4 into `container+0x18`, 28 bytes each, count at
`container+0x14` — the container itself has not been found yet, and with it the
evaluator that actually fires an effect.

### Still unmapped

The BRAKE. Not in the engine's input block (+0x84/+0x88 stay zero while
braking), not in the mirror at 0x430..0x45C either. It will need the same
behaviour-driven hunt that found the tachometer: sample while the pedal moves,
then look for the field that follows it.

## Career: the current car

`0x863480` holds the ID of the career car in use. Not a slot index — an id, and
that is better: it survives the garage being reordered.

The disassembly confirms the role. In 29 places the shape is the same:

    mov eax, [0x863480]     ; the id
    push eax
    mov ecx, 0x83AD90       ; the garage container
    call 0x503680           ; look up by id

It is valid in menus as well as in a race, because it is career state, not race
state. That distinction is what sank two earlier attempts: the car mirror
(player+0x04) is built when a race starts and carries nothing about the garage —
a field there that looked like a car index (+0x28) turned out to be the player's
index among the racers, and read the same for two different cars.

### How it was found

By memory scanning, with the tools now in the SDK:

1. `speed.mem.scan(value)` on a number the player knows (career money), then
   `scanNext` after spending some: converged to `0x861E74`, the money itself.
   Useful, but money is a lone global - there is no struct around it to walk.
2. `speed.mem.scanSnapshot()` over the module globals, then alternating
   `scanDiff(true)` after switching cars and `scanDiff(false)` after a few
   seconds of doing nothing.

The alternation is the part that matters. Filtering only by "changed" stalls at
around 800 survivors - clocks, frame counters and RNG change on every pass and
have nothing to do with cars. They die in an interval where nothing happened, so
"unchanged" is what removes them. Two rounds of that left a single address.

### Storage

Per-car data (tank and odometer) lives under that id, in
`scripts\FRSModLoader\data\pops.json`. The data folder sits outside `mods\`
because installing a mod copies `mods\` over the top, and a save in there would
be wiped by an update of the mod that wrote it.

## Career: the car's model

The garage car entry does not hold a model index. It holds a TYPE HASH at
`+0x20`, and the model table is searched by that hash. Treating the number as an
index is what made a 350Z report itself as "MIATA" - two unrelated values.

The game's own lookup, at `0x610130`:

    mov esi, [0x8A1CCC]       ; the model table
    lea ecx, [esi + 0xD0]     ; each entry's type hash
    cmp [ecx], edx            ; the hash being looked for
    add ecx, 0x890            ; entry size
    cmp eax, 0x2E             ; 46 models

So:

    model table   [0x8A1CCC], 46 entries of 0x890 bytes
    entry + 0x00  the model name, as a string ("350Z", "RX8", "SKYLINE")
    entry + 0xD0  the type hash
    car   + 0x20  the same hash, on the garage entry

And the garage entry itself comes from the game's own lookup:

    speed.call(0x503680, [0x83AD90, carId], { conv: "thiscall", ret: "int" })

which is the call the game makes in 29 places. Entries are 0x7F8 apart, in a
static array; `+0x08` is the car id and `+0x0C` looks like a top speed (274,
196, 317, 309 as floats, one per car).

The name is a better key for per-model data than the index: a mod that changes
the car list would shuffle indices, while a table keyed by name still reads.

### What this is used for

Tank capacity and thirst are per MODEL (a 350Z has 76 litres in any garage);
litres in the tank and the odometer are per CAR (`speed.store.car`). Getting
that split wrong would make a refuel follow the model rather than the car.

## The minimap: the map and the race starts

The stock minimap is not drawn from the world, it is a texture:
`TRACKS\TRACKMAP<id>.BIN` is a pack holding one 512x512 DXT3 of the same name.
**`TRACKMAP4000` is the free-roam city** with no route highlighted; the others
are the same picture with one race's route painted on. Every L4RA city map uses
one framing, world to pixel:

```
px =  0.0917 * x + 312
py = -0.0917 * y + 298.5
```

The DXT3 alpha of every TRACKMAP is all zero - the game draws them opaque - so
`GameTextures` makes a DXT3 with no alpha at all opaque, and loads a TRACKMAP
pack the first time a page asks for `http://nfsu2.tex/TRACKMAP<id>`. The frame
around it and the icons (`MINIMAP_BORDER`, `MINIMAP_MASK`, `MINIMAP_ICON_CAR`,
`MINIMAP_NORTH_INDICATOR`, `MINIMAP_ICON_CIRCUIT`...) are in `InGameCommon.bun`,
which was already loaded.

The race starts (formats in FRSWorldEditor's `NFSU2-FORMATS.md`): the career
chunk `0x80034A10` of `GLOBALB.BUN` lists 224 races, each naming a trigger by
hash; the trigger is a type `0x0D` zone in `0x3414A` of
`ROUTESL4RA\PathsFreeRoam.bin`, whose centre is where the start icon sits. 222
races resolve to 116 points (the two `DDAY` intros have none).

`tools/minimap_dev.py` dumps all of this into `build\minimap-dev\` - textures
with alpha, `events.json`, and the triggers drawn over TRACKMAP4000 to check
the framing. It is for looking at while developing; the mod reads the textures
from the game at runtime and ships none of them.

### Sharp roads: the GPS's own road graph

The TRACKMAP is 512x512 for the whole city, and any zoom a radar needs blurs
it. The roads the game routes its GPS on are vectors, and they are the ones
drawn instead.

Found through the code: the GPS (`0x81CBC0`) is an incremental A* -
`0x413120` hands the car and the target to `0x40C440`, which only files the
request; `0x40C4D0` keeps the open list sorted by g + h, and the expansion turns
node pointers into indices with `sub [0x883DB0]` / `sar 5`. `0x5DEB80` fills
those globals from chunks `0x34121`..`0x34124`, and the file holding them is
`TRACKS\ROUTES<region>\RoutesFreeRoam.bin` (there is one per race too,
`Routes<id>B/F.bin`):

| Chunk | Global | What |
|---|---|---|
| `0x34121` | through `0x5DE1C0` | the streets: 855 records in L4RA, back to back |
| `0x34122` | `[0x883DB0]`, count `[0x883DB4]` | the planner's nodes, 32 bytes, after a 4-byte lead (3289) |
| `0x34123` | `[0x883DBC]`, count `[0x883DC0]` | 276-byte records |
| `0x34124` | `[0x883DC4]`, count `[0x883DC8]` | 264-byte records (absent in L4RA's free roam) |

A street record (`0x34121`), which is all the minimap reads:

```
+0x00  u32 0x0B, 0x0B
+0x10  char[32] name       "TrackRoutesA10"
+0x34  u16 point count     (twice)
+0x60  f32 x4              plan box
+0x84  u8                  1 = highway: the ring and its approaches (55)
+0x85  u8                  1 = avenue, the main streets (225, highways included)
+0x8C  points, 56 bytes    f32 x, y; u8 +0x0E is 252 on nearly every street
```

The classes were found by drawing each candidate byte over the map, one panel
each. `+0x84` isolates the highways with no noise. Point byte `+0x0E` at 248 or
253 on most of a street's points marks 52 short pieces beside the streets -
connectors, loops, passages the TRACKMAP does not draw - which the minimap
draws as alleys; that reading still needs confirming in the game. Other bytes
that turned out NOT to be a road class: `+0x86` and `+0x8A` (large mixed sets),
point `+0x13` (a district: the whole mountain has one value), and header
`+0x71` bit 2 / point `+0x0F` / `+0x1D`, which mark the short pieces inside
junctions.

Walking them by count lands exactly on the chunk's end. Unlike the race
routes, which were the first attempt, it has the alleys, and every street
once instead of one line per race that uses it. How the 32-byte nodes connect
the streets is the next thing to read, for a GPS line of our own.

`src/ui/GameData.cpp` builds the JSON at runtime from the game folder and
serves it next to the textures on a second host:

- `http://nfsu2.data/L4RA/roads.json` - one polyline per street, world units,
  each with its class: `{"c": "highway" | "avenue" | "street" | "alley", "p": [x, y, ...]}`.
- `http://nfsu2.data/L4RA/events.json` - 116 starts, 222 races, the same as
  `tools/minimap_dev.py` finds, checked value by value.

The minimap drew these as SVG for a while and stopped: tuning widths, tones
and junctions by hand to look like a map was a project of its own, and never
looked as good as a drawn one. What it draws now is the TRACKMAP itself, and
the sharpness comes from "NFSU2 Detailed Map" (JeansBig): every TRACKMAP
redrawn at 2048x2048 DXT5, with the missing shortcuts added. It keeps the
original framing exactly - scaled back to 512 it matches the stock map best at
an offset of zero - so the world-to-map formula holds.

It is not installed into the game (that is Binary rewriting every TRACKMAP
and GLOBALB.LZC): the minimap carries `maps\TRACKMAP4000.dds` in its own
folder, and `GameTextures` decodes it straight from there -
`http://nfsu2.tex/mod/<id>/<path>.dds`, the same DXT code as the packs, only
`.dds`, only under the mods folder, no `..`. Checked against Pillow's decode of
the same file: mean difference under 0.3 per channel, i.e. rounding. The page
falls back to the game's own TRACKMAP4000 when the file is not there. The road graph stays in GameData for what it is really for:
routing and drawing a GPS line.

One cost to keep in mind: GameTextures hands the page an uncompressed PNG, and
a 2048x2048 one is 16 MB, built once and cached for the session.

Two things about the 3D in CSS that cost a test each:

- An element flat in the same plane as the tilted map fights it for depth: the
  car arrow flickered because the plane passes exactly through its point. The
  arrow now lives outside the 3D context, and the map and its roads are one
  flat layer, so nothing is coplanar with anything else.
- The race pins are children of the plane that undo its transform in reverse
  (`scale(1/s) rotate(-r) rotateX(-tilt)`), which makes them stand upright,
  face the camera and keep their screen size, while perspective still shrinks
  the far ones.

### Which map: the loaded track's TrackInfo

`[0x883DAC]` is the TrackInfo of whatever the world was built for - free roam
or a race. `0x57F240` sets it while loading, from `GetTrackInfo` (`0x5D3E40`)
over `TheRaceParameters` (`0x89E7A0`). The records live in chunk `0x34201` of
GLOBALB.BUN, 296 bytes each, 125 of them:

| Offset | What |
|---|---|
| `+0x00` | description, `"(4011) Short Circuit #1"` |
| `+0x40` | region, `"L4RA"` |
| `+0x8A` | u16 track id - 4000 is the city's free roam |
| `+0xAC` | f32 TRACKMAP calibration: world x of the top-left corner |
| `+0xB0` | f32 world y of the top-left corner |
| `+0xB4` | f32 width of the map, in world units |
| `+0xC0` | f32 radar zoom: 1.5 city, 2 drag, 1 drift and Street X |

So a map pixel on the 512 TRACKMAP is `px = (x - ulx) / width * 512`,
`py = (uly - y) / width * 512`. For 4000 that is K = 0.0909, CX = 311.3,
CY = 298.8 - close to what was fitted by eye (0.0917 / 312 / 298.5), and
measurably better: the road graph's points land on brighter map pixels (mean
135 against 126). The minimap reads it live, so every track frames itself.

Checked by drawing each region's road graph over the redrawn maps: drift
(4301, L4RC) and Street X (4601, L4RF) fit exactly. **Drag does not**: a drag
TRACKMAP shows the whole city with the strip drawn on it, but the drag tracks
are their own region (L4RB) with their own coordinates, so the car does not
move over that picture. A straight line barely needs a radar; left as is.

The zoom factor is applied to map pixels, not to world units: at constant
world scale a drift track, 739 units across, would be a speck in the middle
of the radar.

### The redrawn maps and their own framing

The minimap carries all 115 of the pack's maps (the Default variant where there
is a choice, the pack's root for drift 43xx and Street X 46xx, which have
none): 461 MB, next to `mods/minimap/maps/`. Most keep the game's framing. Two
groups do not, and the pack fixes them with GLOBALB edits in its `script.end`
that the minimap does not make:

- **URL circuit, 4701-4709.** The pack moves the circuit into an enlarged inset
  in the top-right corner, and edits `TrackMapCalibrationOffsetX/Y` and
  `TrackMapCalibrationZoomIn`.
- **URL airport, 4711-4716.** It edits `TrackMapCalibrationOffsetY` only.

The script's numbers cannot be copied as they stand: for the airport,
`OffsetY 3500` is the top-left y in world units (+0xB0), and the fit agrees
(3494.7), but for the circuit `OffsetX 160` frames nothing - the route has
to sit at x = -4935 for it to land on the drawn loop. So `tools/
minimap_calibrate.py` measures instead: it draws each race's route (`Paths<id>.bin`,
0x34148) over its map and scores the brightness under it, since a race map
paints its own route white. Only the tracks the script recalibrates are
refitted; tracks that share one edit share one fit, and only the params the
script touches move. The result is `maps/calibration.json`, which the page
applies to the redrawn map only - the game's own TRACKMAP keeps the game's
TrackInfo.

| Group | Fit | Route brightness |
|---|---|---|
| 4701-4709 | ulx -4934.7, uly -1411.7, width 4991.0 | 41 -> 193 |
| 4711-4716 | uly 3494.7 (ulx, width unchanged) | 55 -> 251 |

Everything else scores 120-255 with the game's calibration. The lower ones
(4041, 4141, 4174...) are not misframed: the route file joins its segments
with straight lines the map does not paint, and some maps highlight only part
of the route. Drag (42xx, 44xx) scores 40-80 for the reason already noted -
its coordinates are not the city's.

### What the career shows right now

The pause map does not draw every event: `0x4E0440` walks the map event
table and keeps only what `0x533110` approves. The minimap asks the same
question, every two seconds in the city's free roam.

| | |
|---|---|
| `[0x850078]` | 1 while the table is valid; `0x500EE0` (the iterator) returns nothing otherwise |
| `0x85AD40` | 64 entries of 0x18 bytes. +0 zero = empty; +4 a race (the 136-byte career record, trigger hash at +0x28); +8 a shop (160 bytes, zone hash at +0x3C); +0xC and +0x10 two other kinds (0x501150 on 0x85EF50, 0x5135A0 on 0x861E88) |
| `0x533110` | `thiscall(entry) -> al`. Races go to `0x532C20` (race+0x7F, +0x0D, 0x5007E0 on table+0x7148). Shops: byte +0x9D (the stage it opens in) must not exceed `[0x862838]`, and a real shop (+0x51) must be found as **discovered** - `0x5003B0(name hash at +0x38)` on the same table, record +4 == 1 - then `0x512870` (+0x9C) |

Two traps: `0x533110` returns in AL and leaves the rest of EAX as it was, so
`speed.call` is asked for `int` and masked with `& 0xFF` (the `bool` return
tests the whole register). And `shops.json` now carries each shop's zone hash,
which is what the table's shop record names.

Before a career is loaded the table is not valid and the minimap shows every
start and shop, as before.

### Hiding the stock minimap, and nothing else

The stock minimap is not a widget of its own: it is loose FEng objects in the
HUD package (`HUD_SingleRace.fng`, 111 of its 258 objects; `HUD_Short_Track.fng`
has 19), which the minimap's constructor `0x4F73E0` finds by name with
`FEHashUpper` (`0x505450`) + `FEngFindObject` (`0x5379C0`). The HUD builds it
at `0x4F8BF0` and keeps it at `HUD+0x5C`, the HUD itself at `player+0x9A4`.

Named by hashing and matching against the package:

| Name | What |
|---|---|
| `Track2000_map` (90FE80F3) | the map image (resource Track4000_map.tga) |
| `TrackMapTargetRing` (382D2FC9) | a group: the four quarters of the border |
| `Minimap_Mask`, `Minimap_North_indicator`, `StartLineIndicator`, `PlayerCarIndicator_1/2`, `GPS_Search_Group` | the rest of the frame |
| `<prefix><n>` from the table at `0x7F4348` | the event and shop icons, up to ~10 each |
| D80C97E1, 7AF1CA61..63, 697D2BCA | unnamed: a backing, three start-line icons, the GPS selector group |

The minimap mod hides exactly those: `0x52CEF0(name)` says whether a HUD
package is loaded; when its address changes the objects are resolved again,
and every frame any of them that is visible goes through `FEngSetInvisible`
(`0x50CA00`: sets bit 0 of `+0x1C`, and for a group also its children).
Pointers are dropped when the package goes, so nothing freed is touched.
Whether the game's own per-frame update shows an icon again before the frame
is drawn is the thing to watch: if it does, the icons flicker.

### The GPS route

The GPS (`0x81CBC0`) is an incremental A*. `0x413120(car, &target)` files the
request through `0x40C440` (start at +0x10, target at +0x30, state +0 = 1);
`0x4268A0` is its update:

| State | |
|---|---|
| 1 | `0x422B90` sets the search up |
| 2 | `0x422F90` steps it: open list at +0x44 (count +0x16C4), closed at +0x2C4 (count +0x16C8); a search node holds the road node at +4, g at +8, h at +0xC |
| 3 | done: if +0x17A0, the route built at +0x16E0 is handed to the requester (+0x17A4, the car) by `[[car+0x2C]]+0x4C(code, &route)`, then the GPS resets |

That method is `0x41EA30`: the code goes to `ctrl+0x3E4` and the route - a
0xC0-byte struct, copied by `0x40A2D0` - to `ctrl+0x3F0`. In the route, +0x38
holds up to 64 u16 and +0xB9 how many; `0x418970` reads them as
`value & 0x1FFF` = the index of a node in `0x34122` (the `sub [0x883DB0]` /
`sar 5` of a node pointer), and `0x2000` as a direction bit.

A 0x34122 node (32 bytes, after a 4-byte lead) is the passage from one street
into another: street id +0x08 left at its point +0x04, street id +0x0A entered
at its point +0x06 - the street id being +0x0A of a 0x34121 record, 101..1219
in L4RA, one per street. Checked on all 3289 nodes: the two points are always
within 40 units (median 2.2). So a route draws itself: along each street, from
the point it was entered to the point the next node leaves it; before the
first node, from the car's nearest point on the street it is on; after the
last, to the point nearest the GPS target.

`graph.json` serves the nodes as `[street, point, street, point]`, and
`roads.json` now carries each street's `id`. The minimap reads
`[[player car]+0x2C]+0x428/+0x4A9` four times a second and redraws the line
when the node list changes.

Hiding every frame was not enough for one thing: with a GPS route set, the
target's pulsing selector (`Minimap_GPS_Selector`, inside groups 697D2BCA and
E2848973) is made visible again after the main-loop hook and before the frame
is drawn, so it kept showing. Two changes fixed the rest of the stock minimap
for good:

- Groups are walked: an FEng group is type 5 at `+0x18`, with its child count
  at `+0x60`, the first child at `+0x64` and each child's next at `+4` (the loop
  in `0x50CA25`). Hiding a group hides its children only at that moment, so
  the children go in the list themselves.
- `FEngSetVisible` (`0x50CA50`) gets a detour to a stub in memory from
  `speed.mem.alloc`: it looks the object up in a table of the stock minimap's
  objects (`[count][object...]`, rewritten by the mod whenever a HUD package
  comes or goes) and returns without touching it; anything else runs the
  original from the `je` at `0x50CA56`. It is only installed if the function
  still starts `8B 44 24 04 85 C0 74 49`, i.e. nobody else hooked it.

### The pause map's filters

The legend of the pause map turns each kind on and off, and the stock minimap
obeys it. It is one byte per category, 1 = shown; the order comes out of three
of the game's own tables, not out of the kind numbers:

| | |
|---|---|
| `0x5002E0` | race kind: `+0x0D == 1` -> 7 (SUV); `+0x34 == 2` -> 5 (URL; `+0x34 == 1` is a special race, drawn as an X); else by race type `+0x0F`: 0 circuit -> 0, 1 sprint -> 4, 2 Street X -> 2, 4 drag -> 1, 5 drift -> 3 |
| `0x500340` | shop kind by `+0x50`: 1 paint -> 9, 2 body -> 10, 5 specialty -> 11, 3 performance -> 12, 4 car lot -> 13, 0 garage -> 14 |
| `0x4965F0` | kind -> category: 0-5 as is, 7 -> 6, 9 -> 7, 10 -> 8, 11 -> 11, 12 -> 9, 13 -> 10, 14 -> 12 |
| `0x4964D0` | category -> byte (the getter; `0x496390` the setter) |

End to end, by the minimap's kind names: CIRCUIT `0x8635A9`, DRAG `AA`, STREET
`AB`, DRIFT `AC`, SPRINT `AD`, URL `AE`, SUV `B0` (checked in the game: kind 5
is URL and kind 7 is SUV, the opposite of the first reading), PAINT
`B2`, BODY `B3`, SPECIALTY `B4`, PERFORMANCE `B5`, CAR LOT `B6`, GARAGE `B7`.

The minimap does not use that table itself any more. Mapping kinds by hand
got URL and SUV wrong (race `+0x0D == 1` is SUV, kind 7; `+0x34 == 2` is URL,
kind 5), and a
trigger with races of several kinds showed as one pin of the most common
kind. It now asks the game, entry by entry of the map event table: `0x533110`
(shown), `0x500DE0` (thiscall, the entry's kind), `0x4965F0` (kind ->
category), `0x4964D0` (category -> 1 if the legend hides it). Each race left
is identified by its trigger (+0x28) and track (+0x18 low word) - the track
picks, among the trigger's races, the one that is available, and so its kind -
and special races get their own icon. A special race - the X on the map, of
any kind - is race `+0x34 == 1`: that is the test `0x4B06D0` makes before
giving the indicator the `MINIMAP_ICON_CROSS` texture (B90F50BB).

**The route's indices are into the table in memory, not the file.** The
nodes the game has at `[0x883DB0]` (count `[0x883DB4]`) are not the file's:
3273 in L4RA against the file's 3289, numbered differently - memory node 959
is file node 957. Reading the file's table with the route's indices gave the
nodes that seemed "off the path" and the route that kept needing repairs.
Read from memory, the same route (8 nodes) runs in order from the car to the
target - the distance to the car grows 143 -> 524, to the target it falls
488 -> 30 - with nothing off the path. The fields inside a node are the same
(streets +8/+0xA, points +4/+6, and street ids match `roads.json`), so the
minimap reads each route node's four fields from memory and sends them to the
page; `graph.json` is still what fills the short gaps between them. The route
holds decision points, not every passage: consecutive nodes rarely share a
street, and the gaps are 30-130 units.

### Turning with the camera

The radar turns with the camera, GTA-style, not with the car. The view matrix
is at `0x8734A0` (the one `mods/pops` found with its `/camera` command). The
camera's forward in the ground plane is its **third column, m[2] and m[6],
positive**: measured in the game by scoring the four candidates (third column
or third row, each with both signs) against the car's heading while driving
over 30 km/h with the camera behind - 0.94 for this one. The measurement was
done once and the result is now fixed in the code, so the radar turns with the
camera from the first frame at any speed. The player's arrow turns by the car's
heading minus the camera's; `/minimap giro carro` goes back to turning with
the car.

### The HUD's GPS arrow

The blue arrow that floats ahead of the car with a GPS route is not FEng: it is
a marker drawn in the world with the texture `MARKER_GPS_AID`, which
`0x5F3E10` loads into `player+0x110` (and `MARKER_DIRECTION_AID` into
`+0x10C`). `0x5F3CB0` (thiscall, one argument, `ret 4`) draws it: it checks
the controller's route state (`ctrl+0x3E4` 1 or 2), finds a point ahead on the
route and draws through `0x5EAC90`. Its only caller, `0x63176A`, ignores the
result, so hiding the arrow is making the function return at once
(`C2 04 00` over `55 8B EC`) and showing it again is putting the bytes back.
The minimap does that on `/minimap seta esconder|mostrar`, and only if the
function starts with one of those two sequences.

`mod/<id>/<path>.dds` also looks **inside the zips of the mod's root** when
there is no loose file: a mod can ship a texture pack as downloaded. The entry
is found by file name (a `Default` folder preferred, `Beta` skipped), stored
or LZMA-compressed (zip method 14, 7-Zip's), and decoded with the LZMA SDK's
`LzmaDec` in `third_party/lzma` (public domain). Each zip's central directory
is read once. FRSMiniMap reads the "NFSU2 Detailed Map" maps this way.
