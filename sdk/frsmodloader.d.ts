// FRSModLoader SDK types, for editor completion.
//
// In a mod's main.js (QuickJS, inside the game):
//   /// <reference path="../../sdk/frsmodloader.d.ts" />
//
// An address is always a `number`: the game is 32-bit, so any pointer fits in a
// double without loss. `null` means "cannot read right now" - the object does
// not exist yet, the pointer is null, or the memory is not mapped.

declare namespace speed {
    /** FRSModLoader version. */
    const version: string;

    /** The mod currently running. */
    const mod: {
        readonly id: string;
        readonly name: string;
        /** The mod's folder, for building file paths. */
        readonly dir: string;
    };

    /**
     * Everything FRSModLoader has loaded, in load order.
     *
     * Called from the top level of your own main.js it returns a partial list:
     * your mod is in it, but whoever loads after you is not there yet. From a
     * frame or an event the list is complete.
     */
    function mods(): { id: string; name: string; ui: boolean }[];

    /** Milliseconds since Windows booted (GetTickCount). */
    function now(): number;

    // ----------------------------------------------------------------- events
    /**
     * - `"frame"`        - every game frame, on the game thread. Keep it light:
     *                      whatever goes here runs 60+ times per second.
     * - `"gamestate"`    - the gameflow changed: `{ state, previous }`.
     * - `"keydown"`      - a key pressed while the UI does not have focus:
     *                      `{ key }`, the Windows virtual-key (0x71 = F2).
     * - `"menu"`         - an item added with speed.menu.add was chosen:
     *                      `{ id }`.
     * - `"ui"`           - any message from the UI: `{ channel, data }`.
     * - `"ui:<channel>"` - only that channel; receives the data directly.
     */
    function on(event: "frame", cb: () => void): void;
    function on(event: "gamestate", cb: (e: { state: number; previous: number }) => void): void;
    function on(event: "keydown", cb: (e: { key: number }) => void): void;
    function on(event: "menu", cb: (e: { id: number }) => void): void;
    function on(event: "ui", cb: (e: { channel: string; data: any }) => void): void;
    function on(event: string, cb: (data: any) => void): void;
    /** Without `cb`, removes every listener for that event. */
    function off(event: string, cb?: Function): void;
    /**
     * An event for the other mods: every other mod's `speed.on(event)` gets
     * `data` (as JSON, so plain values only) at the start of the next frame.
     * The name needs a colon, e.g. `"fuel:state"`, so it cannot be mistaken
     * for one of the loader's events. Not delivered to the sender.
     */
    function emit(event: string, data?: any): void;

    // ------------------------------------------------------------------- game
    namespace game {
        /** 1 boot, 3 frontend, 4/5 loading, 6 gameplay. */
        function state(): number;
        function stateName(): "boot" | "frontend" | "loading" | "gameplay" | "unknown";
        function isRacing(): boolean;
        function hasFocus(): boolean;

        /** The player object, or null outside a race. */
        function player(): number | null;
        /**
         * The car mirror (player+0x04): the copy the game keeps for its dial
         * and engine sound. Great to read, useless to write — physics ignores
         * it.
         */
        function car(): number | null;
        /**
         * The gearbox snapshot (car+0x34 -> +0x18). It is only written on a
         * shift and freezes in between: good for the gear (+0x34) and for
         * detecting the moment of a shift, not for live RPM.
         */
        function gearbox(): number | null;
        /**
         * The vehicle physics object, captured by the hook at 0x5A5540.
         * Unlike the mirror, this is where writing has an effect.
         */
        function physics(): number | null;
        /** The engine object (physics+0x48): boost, rev limit, input throttle. */
        function engine(): number | null;
        /** HWND of the game window. */
        function window(): number | null;

        /** A live read of the car mirror, or null outside a race. */
        /** The id of the career car in use, or null outside a career. */
        function carId(): number | null;

        /**
         * The model name: "350Z", "RX8", "SKYLINE".
         *
         * It comes from the game's own lookup (0x610130): the garage car
         * holds a type HASH, and the model table is searched by it — not
         * indexed. Treating the number as an index makes a 350Z present
         * itself as "MIATA".
         *
         * It works as a key for per-model data (tank, consumption, tuning),
         * and it beats the index: a mod that changes the car list shuffles
         * indices, but a table keyed by name stays valid.
         */
        function carModel(): string | null;

        function telemetry(): {
            /** m/s, the way the game stores it. */
            speed: number;
            kmh: number;
            mph: number;
            /** 800 at idle up to ~7000. */
            rpm: number;
            /** 0 / 0.7 / 1.0 on keyboard. */
            throttle: number;
            /** Metres travelled. */
            distance: number;
            /**
             * The raw gear from the gearbox snapshot, null if it does not exist
             * yet. The game's count is off by one: **1 is neutral**, 2 is first
             * gear, and so on (0 is reverse).
             */
            gear: number | null;
            /** Boost pressure; negative under vacuum. null with no engine object. */
            boost: number | null;
            /** This car's ceiling; 0 when naturally aspirated. */
            boostMax: number | null;
            /** The vacuum floor, the negative end of the scale. */
            boostMin: number | null;
            /** Idle speed for this car, 800 or 850. */
            idle: number | null;
            /** Rev limit for this car, 7000 or 7500. */
            redline: number | null;
        } | null;

        /** Offsets inside the car mirror, for reads the SDK does not wrap. */
        const offset: {
            readonly speed: number;
            readonly throttle: number;
            readonly rpm: number;
            readonly distance: number;
        };

        /** FEHashUpper: the hash the frontend uses for event and screen names. */
        function hash(text: string): number;
        /** Injects an event into a frontend package. */
        function sendFrontendMessage(hash: number, packageName: string): void;

        /** Raw addresses, for going past what the SDK wraps. */
        const addr: {
            readonly gameFlowManager: number;
            readonly hWnd: number;
            readonly isLostFocus: number;
            readonly windowedMode: number;
            readonly skipMovies: number;
            readonly feng: number;
            readonly playersByNumber: number;
        };
    }

    // ------------------------------------------------------------------- menu
    /**
     * Items in the game's own main menu.
     *
     * These are real IconOptions, built with the engine's own
     * IconOption_Create and added through IconScrollerMenu::AddOption, so they
     * scroll and look like the game's. Choosing one fires the `"menu"` event
     * with the id `add` returned.
     *
     * The label is the rough edge: an option's text and icon are hashes
     * resolved against Languages\*.bin and the texture table, so a word of
     * your own means adding a string to the language file. Until then, pass a
     * key that already exists, or an explicit `labelHash`.
     */
    namespace menu {
        function add(item: {
            /** A language key; hashed with bStringHash. */
            label?: string;
            /** An explicit string hash, used instead of `label`. */
            labelHash?: number;
            /** Texture hash. Defaults to one the main menu already uses. */
            icon?: number;
        }): number;

        /** bStringHash, for working out the hash of a key yourself. */
        function hash(key: string): number;
    }

    // ----------------------------------------------------------------- memory
    namespace mem {
        function readU8(addr: number): number | null;
        function readI8(addr: number): number | null;
        function readU16(addr: number): number | null;
        function readI16(addr: number): number | null;
        function readU32(addr: number): number | null;
        function readI32(addr: number): number | null;
        function readPtr(addr: number): number | null;
        function readF32(addr: number): number | null;
        function readF64(addr: number): number | null;
        function readString(addr: number, max?: number): string | null;
        function readBytes(addr: number, length: number): ArrayBuffer | null;

        function writeU8(addr: number, value: number): boolean;
        function writeU16(addr: number, value: number): boolean;
        function writeU32(addr: number, value: number): boolean;
        function writePtr(addr: number, value: number): boolean;
        function writeF32(addr: number, value: number): boolean;
        function writeF64(addr: number, value: number): boolean;

        /**
         * Patches bytes, handling page protection for you.
         * Takes `[0x90, 0x90]` or `"90 90 E9 ?? ??"` (`??` = leave alone).
         */
        function patch(addr: number, bytes: number[] | string): boolean;
        /** Fills with NOPs. */
        function nop(addr: number, length: number): boolean;

        /**
         * Walks a pointer chain and returns the final address (without reading
         * the value there): `chain(0x8900AC, [0x04, 0x34, 0x18])`.
         */
        function chain(base: number, offsets?: number[]): number | null;

        /** Executable memory, for your own trampolines and stubs. */
        function alloc(size: number): number | null;
        function free(addr: number): boolean;
        /** Is that memory mapped and readable? */
        function valid(addr: number, size?: number): boolean;

        /**
         * Repoints an existing CALL rel32; returns the previous target, so you
         * can chain. Throws if the address does not start with E8.
         */
        function redirectCall(site: number, target: number): number;
    }

    /**
     * Calls a function in the game.
     *
     *   speed.call(0x53FEB0)                                   // race launcher
     *   speed.call(0x5FAE20, [player], { conv: "thiscall" })   // autopilot
     *   speed.call(0x505450, ["UI_Main"], { ret: "int" })      // FEHashUpper
     *
     * A string becomes a `char*`. A fractional number becomes a float (IEEE
     * bits on the stack); an integer stays an integer.
     */
    function call(
        addr: number,
        args?: (number | string | boolean)[],
        options?: {
            conv?: "cdecl" | "stdcall" | "thiscall";
            ret?: "int" | "float" | "double" | "bool" | "void" | "string";
        }
    ): number | string | boolean | null | undefined;

    // ------------------------------------------------------------------ Audio
    /**
     * A mod's own sounds, fired from the game thread.
     *
     * Native (XAudio2) rather than an <audio> tag in the UI: these sounds
     * answer to physics, and a pop belongs to the gear change that caused it.
     * The UI layer is a process away, behind JSON and a message queue - right
     * for interface sounds, wrong for engine noises.
     *
     *   const pop = speed.audio.load("sounds/pop1.wav");   // relative to the mod
     *   speed.audio.play(pop, { volume: 0.8, pitch: 1.1 });
     */
    namespace store {
        /** Reads one of the mod's values; `fallback` when it does not exist. */
        function get(key: string, fallback?: any): any;
        /** Writes and persists right away. */
        function set(key: string, value: any): void;
        function all(): any;
        function clear(): void;

        /**
         * The same, but PER CAREER CAR.
         *
         * The key is the id the game uses for the car in use (0x863480), not
         * a garage index: it survives the garage being reordered, and it is
         * valid in the menu as much as in a race.
         *
         *   speed.store.car.set("odometer", 1240.5);
         *   const km = speed.store.car.get("odometer", 0);
         *
         * With no car loaded (main menu before a save), `get` returns the
         * fallback and `set` does not write — better not to save at all than
         * to save to the wrong car.
         */
        namespace car {
            function get(key: string, fallback?: any): any;
            function set(key: string, value: any): boolean;
            /** The id of the car in use, or null. */
            function id(): number | null;
        }
    }

    namespace audio {
        /**
         * Loads a PCM WAV and returns its id, or null if it could not be read.
         * A relative path is resolved against the mod's folder. Loading the
         * same path twice gives the same id back, not a second copy.
         */
        function load(path: string): number | null;

        /**
         * Fires a sound. `volume` is 0..1; `pitch` is a frequency ratio, so
         * 0.5 drops an octave and 2 raises one (clamped to 0.03..4).
         *
         * Returns false if the id is unknown or every voice in the pool (16)
         * is still busy - so overlapping pops degrade by dropping one, never
         * by cutting another short.
         */
        function play(id: number, options?: {
            volume?: number; pitch?: number;
            /** Repeat the whole sound, sample-exact, until stopped. */
            loop?: boolean;
            /** Rise from silence over this many ms. */
            fadeIn?: number;
        }): boolean;

        /** Silences one sound; with no id, everything. `fade` goes down over that many ms first. */
        function stop(id?: number, options?: { fade?: number }): void;
        function stopAll(): void;

        /** Frees the sound's memory and its voices. */
        function unload(id: number): boolean;

        /** Reads the master volume, or sets it and returns the new value. */
        function volume(value?: number): number;

        /** Is the engine up? It starts itself on the first load(). */
        function ready(): boolean;
    }

    // --------------------------------------------------------------------- UI
    namespace ui {
        /** Sends data to this mod's page. */
        function send(channel: string, data?: any): void;

        function show(): void;
        function hide(): void;
        /** Returns the new visibility. */
        function toggle(): boolean;
        function visible(): boolean;

        /**
         * Hands keyboard and mouse to the UI (same as the key in the ini).
         * With `{ mouse: false }` only the keyboard goes: no cursor appears
         * and the mouse stays with the game - for screens driven like the
         * game's own, with arrows, Enter and Esc.
         */
        function capture(on?: boolean, options?: { mouse?: boolean }): void;
        function capturing(): boolean;

        /** Reloads the page; with `true`, ignoring the cache. */
        function reload(hard?: boolean): void;
        /** Opens Chromium's DevTools. */
        function devtools(): void;
        /** Layer size, in backbuffer pixels. */
        function size(): { width: number; height: number };

        /**
         * One of the game's own menu sounds, by what it is for. False when
         * the frontend has no audio yet. A page plays them itself with
         * `frsmodloader.sound(name)`; see docs/UI-KIT.md, "Keyboard and sounds".
         */
        function sound(name: MenuSound): boolean;
    }

    type MenuSound = "up" | "down" | "left" | "right" | "valueLeft" | "valueRight" |
                     "confirm" | "open" | "close" | "wrong";

    // --------------------------------------------------------------- settings
    /**
     * The mod's options, as the player set them in Options > Mods. They are
     * declared in mod.json ("settings": [...]) and drawn by the loader; the
     * mod only reads them. See docs/MODDING.md, "Settings and the Mods menu".
     *
     *   const volume = speed.settings.get("volume");
     *   speed.on("settings", (all) => apply(all));
     *   speed.on("settings:action", (e) => { if (e.id === "test") play(); });
     */
    namespace settings {
        /** The value, or the declared default, or `fallback`. */
        function get(id: string, fallback?: any): any;
        /** Every value, defaults filled in. */
        function all(): Record<string, any>;
        /** Changes one from the mod's side (a hotkey); stored, not announced. */
        function set(id: string, value: any): void;
        /** More rows, decided at runtime; shown the next time the menu opens. */
        function define(items: Setting[]): void;
    }

    type Setting =
        | { type: "section"; label: string }
        | { id: string; type: "toggle"; label: string; default?: boolean;
            on?: string; off?: string; description?: string }
        | { id: string; type: "range"; label: string; min: number; max: number;
            step?: number; unit?: string; default?: number; description?: string }
        | { id: string; type: "choice"; label: string;
            options: (string | number | { value: any; label: string })[];
            default?: any; description?: string }
        | { id: string; type: "action"; label: string; button?: string; description?: string };
}

declare function setTimeout(fn: () => void, ms?: number): number;
declare function setInterval(fn: () => void, ms?: number): number;
declare function clearTimeout(id: number): void;
declare function clearInterval(id: number): void;
