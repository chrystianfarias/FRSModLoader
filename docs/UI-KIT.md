# nfsu2.css — the game's interface, as a stylesheet

Underground 2's menus are a small, consistent system: a translucent black panel
outlined in yellow-green, a bold italic title in that same green, flat grey rows
with the selected one washed green, and pill buttons with a thin bright edge.
`ui/nfsu2.css` is that system as classes, so a mod's panel looks like it shipped
with the game instead of like a web page over it.

Underneath, every surface is flat CSS. Inside the game, the pieces the game
draws from a texture — the pill button and its glow, the arrow rings, the
selected tile's frame, the icons — are drawn with **that texture**, read from
the player's own game files at runtime (see [The game's own textures](#the-games-own-textures)).

It installs with the loader (it lives next to `shell.html`), so there is
nothing to copy into your mod.

**To see it without starting the game, open
[`docs/ui/kit.html`](ui/kit.html) in a browser.** Every component is on that
page, over a dark backdrop that stands in for the road. Edit the CSS, hit
refresh. Outside the game there are no textures, so the page shows the flat
version, and that is what the picture below is.

![The kit](ui/kit.png)

## Using it

```html
<link rel="stylesheet" href="nfsu2.css">

<div class="nfs nfs-at nfs-at--tl">
  <div class="nfs-panel">
    <div class="nfs-panel__head">
      <h1 class="nfs-title">My Mod</h1>
    </div>
    <div class="nfs-panel__body">
      <ul class="nfs-list">
        <li class="nfs-row nfs-row--on"><span class="nfs-row__label">Selected</span></li>
        <li class="nfs-row"><span class="nfs-row__label">Another</span></li>
      </ul>
    </div>
    <div class="nfs-panel__foot">
      <button class="nfs-btn">Back</button>
      <button class="nfs-btn">Continue</button>
    </div>
  </div>
</div>
```

Two things about that `href`. It has no path in it because the shell clones
your `<link>` into its own document, so the URL resolves from `shell.html` —
plain `nfsu2.css` finds the file from any mod, at any depth. And it works
through a shadow root: the stylesheet lands inside yours, and nothing of it
leaks out to another mod.

Everything is scoped under `.nfs`, which has to be on an ancestor of whatever
you style. That is deliberate: a panel of the game's furniture is usually one
part of a mod's screen, not all of it.

## Sizing

One variable. `--nfs-size` is the base font size everything else is measured
from, so a panel scales whole:

```html
<div class="nfs" style="--nfs-size: 22px"> ... </div>
<div class="nfs nfs--sm"> ... </div>   <!-- 12px -->
<div class="nfs nfs--lg"> ... </div>   <!-- 19px -->
```

Worth setting from the page: the same HUD runs at 1024×768 and at 4K, and the
overlay does not scale for you.

## Placing it

Your mod's layer already covers the screen and is click-through. `.nfs-at`
puts a panel in a corner of it:

```html
<div class="nfs nfs-at nfs-at--br"> ... </div>
```

`--tl`, `--tr`, `--bl`, `--br`, `--center`, or set `--nfs-top` / `--nfs-right`
/ `--nfs-bottom` / `--nfs-left` yourself.

A `.nfs-panel` turns clicks back on for itself (`pointer-events: auto`), so a
button inside it is clickable — once the player has pressed **F1** and the UI
has the mouse.

## The classes

### Panel

| | |
|---|---|
| `.nfs-panel` | the outlined, translucent window |
| `.nfs-panel--plain` | one without a head or a foot: just padding |
| `.nfs-panel__head` | the darker title bar |
| `.nfs-panel__body` | the content |
| `.nfs-panel__foot` | the row of buttons at the bottom |
| `.nfs-title` | bold italic, bright green — the screen's name |
| `.nfs-subtitle` | white, centred — what it is showing ("Full Map") |
| `.nfs-label` | small, spaced, uppercase — a field's name |
| `.nfs-caption` | green, centred — the name of what is selected above it |
| `.nfs-rule` | a thin green rule between sections |
| `.nfs-split`, `.nfs-split__side`, `.nfs-split__main` | the World Map's two columns |

### List

| | |
|---|---|
| `.nfs-list` | the stack of bars |
| `.nfs-row` | one bar; alternate rows darken on their own |
| `.nfs-row--on` | selected: washed green, green text - pulsing while the list has the focus (also `aria-selected="true"`) |
| `.nfs-row--head` | a heading row, like "Your Car" |
| `.nfs-row--empty` | the filler the game draws to the bottom of a panel |
| `.nfs-row--clickable` | pointer cursor and a hover state |
| `.nfs-row__label` | the text; it ellipsises |
| `.nfs-row__value` | a value on the right, tabular |
| `.nfs-check` | the tick in its own darker gutter (`.nfs-check--off` keeps the space) |
| `.nfs-dot` | a legend dot: `--green --cyan --yellow --red --blue --purple` |
| `.nfs-tri` | the little triangle that marks a car (`--orange` for the rival) |

### Options sheet

The pages under the game's Options (Audio, Video, Player): a light translucent
sheet, dark italic text, the value in its own column. The selected row is a dark
rounded bar that **stays still**; its text **pulses** in the green. The
circled arrows either side of the value are drawn on that row only, and they
**answer a press**: a click on one, or Left / Right, makes it swell and flash
green once (`.is-pressed`, below). The mouse selects the way the keys do: move
`.nfs-option--on` to the row under the pointer on `mouseenter` (with the
`up`/`down` sound - see [Keyboard and sounds](#keyboard-and-sounds)). The same
goes for a `.nfs-list`: select on hover, and the row under the pointer is always
the lit one.

| | |
|---|---|
| `.nfs-sheet` | the light grey sheet the rows sit on |
| `.nfs-options` | the list of rows |
| `.nfs-option` | one setting: label, left arrow, value, right arrow |
| `.nfs-option--on` | selected: dark bar, green text - pulsing while the list has the focus - arrows shown (also `aria-selected="true"`) |
| `.nfs-option--head` | a heading between groups; not selectable |
| `.nfs-option--dim` | seen, but with no effect right now |
| `.nfs-option__label`, `.nfs-option__value` | the two text cells; a `.nfs-track` fits in the value |

```html
<div class="nfs-sheet">
  <ul class="nfs-options">
    <li class="nfs-option nfs-option--head">Driving</li>
    <li class="nfs-option nfs-option--on">
      <span class="nfs-option__label">Camera</span>
      <button class="nfs-arrow nfs-arrow--left"></button>
      <span class="nfs-option__value">Far</span>
      <button class="nfs-arrow"></button>
    </li>
  </ul>
</div>
```

Keep the arrows on every row: they hold the column, so the values do not move
when the selection does. For a row that acts instead of holding a value, put
empty `<span>`s where the arrows go.

### Pulses

The game's selection neither moves nor glows - its text breathes, and only
where the player is. A selected row pulses while its list has the focus: put
`.is-focus` on the `.nfs-list` / `.nfs-options` the keys are driving (and take
it off the others), or let real focus inside the list do it (`:focus-within`).
Selected rows in any other list stay lit and still - which is also what a list
wants when `--on` marks a state rather than the cursor, like a legend of what
the map shows. An arrow does not breathe: it answers when it is pressed.

| | |
|---|---|
| `.is-focus` | on a list: its selected row pulses |
| `.nfs-pulse` | this text breathes like a selection, focus or not |
| `.nfs-arrow.is-pressed` | one press: the arrow swells and flashes green, once |
| `--nfs-pulse-time` | one breath, `1.05s` |
| `--nfs-press-time` | one press, `.3s` |

A click, and the Left / Right key its side stands for, are both presses. Add
the class, and take it off when the animation ends, so the next press plays it
again:

```js
arrow.addEventListener("animationend", () => arrow.classList.remove("is-pressed"));

function pressArrow(arrow) {
  arrow.classList.remove("is-pressed");
  void arrow.offsetWidth;            // restart it, for a quick second press
  arrow.classList.add("is-pressed");
}
```

A mouse button held down on an arrow keeps it swollen (`:active`) with no code
at all.

`prefers-reduced-motion` turns every animation in `.nfs` off.

### Controls

| | |
|---|---|
| `.nfs-btn` | the pill button; `--wide`, `--block` |
| `.nfs-btn.is-on` | the selected look, for when you drive focus yourself |
| `.nfs-btn[disabled]`, `.is-off` | dimmed, no glow |
| `.nfs-btn-round` | the circular arrow button from a header |
| `.nfs-tiles`, `.nfs-tile`, `.nfs-tile--on` | the Pause screen's row of icons; `--on` is the one glow in the kit |

`:hover` and `:focus-visible` already turn a button's edge and label green,
and in the game lay the button's glow texture over it, so a mod driven by the
keyboard only has to move `.is-on`.

### Game art

| | |
|---|---|
| `.nfs-icon` | any texture of the game as a glyph in `currentColor`; name it in `--nfs-icon`. `--square` for the 1:1 ones |
| `.nfs-arrow`, `.nfs-arrow--left` | the circled arrow button, as the game's own piece of art |

```html
<button class="nfs-tile nfs-tile--on">
  <span class="nfs-icon" style="--nfs-icon: url(http://nfsu2.tex/INGAME_ICON_EXPLORE)"></span>
  Return to Explore Mode
</button>
```

Both exist only in the game: outside it the texture does not load and they are
not drawn.

### Readouts

| | |
|---|---|
| `.nfs-readout` | a big green tabular number, for mid-race reading |
| `.nfs-kv`, `.nfs-kv__k`, `.nfs-kv__v` | a label and its value on one line |
| `.nfs-track`, `.nfs-track__fill`, `.nfs-track__knob` | a bar; set `--value` from 0 to 1 |

```html
<div class="nfs-track" style="--value: .72"><div class="nfs-track__fill"></div></div>
```

## Keyboard and sounds

The game's menus have no cursor. Up and down pick a row, left and right change
what is on it, Enter acts, Esc goes back - and every one of those makes the
game's own little sound. A panel that looks like the game but is driven only by
the mouse still reads as a web page, so the kit comes with both halves: the way
to take the keys, and the sounds.

The loader's **Mods** menu (Options → Mods, `ui/mods.html`) and the frs-world
gas station are built this way; either is a complete example to copy from.

### The sounds

Any page plays them itself - no `main.js` in between:

```js
frsmodloader.sound("down");
```

From a mod's script it is `speed.ui.sound("down")`, and from a native plugin
`api->menu_sound("down")` ([NATIVE_PLUGINS.md](NATIVE_PLUGINS.md)). They are
named for what they are for, not by the game's ids, and played through the
frontend's own audio, at the game's volume:

| name | when |
|---|---|
| `up`, `down` | the highlight moves to another row |
| `left`, `right` | the highlight moves to another button in a row |
| `valueLeft`, `valueRight` | a row's value changes (a choice, a toggle, a slider) |
| `confirm` | Enter on something that acts (a button, an action row) |
| `open`, `close` | a screen of yours comes and goes |
| `wrong` | a key that has nothing to do there (a slider at its end, a locked row) |

A move that goes nowhere still sounds its direction, as the game's do; `wrong`
is for a real "no".

### The keys

Three things are not what a web page expects, and the pattern below handles all
three:

- **Read `e.keyCode`, not `e.key` or `e.code`.** The loader hands Chromium the
  Windows virtual key; `e.code` comes through empty.
- **Auto-repeat is not marked**, so a held key would fire every few frames.
  Count a key once when it goes down, and again only after it has come up.
- **Keys reach the page only while something in it has focus**, and nothing is
  clicked in a menu driven by arrows. Give the panel `tabindex="-1"`, focus it
  when it opens, and take the focus back when it wanders off.

The player's keyboard has to be the page's too: `speed.ui.capture(true)` from
the mod (or `capture_input(1)` from a plugin) when your screen opens, and
`false` when it closes. `{ mouse: false }` keeps the mouse with the game, for a
screen that is only ever driven by the keys.

```html
<div class="nfs nfs-at nfs-at--center" id="menu" tabindex="-1">
  <div class="nfs-panel">
    <ul class="nfs-list" id="rows"> ... </ul>
  </div>
</div>

<script>
  const VK = { BACK: 8, ENTER: 13, ESC: 27, SPACE: 32, LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40 };
  const menu = root.getElementById("menu");
  const rows = Array.from(root.querySelectorAll("#rows .nfs-row"));
  const held = new Set();
  let open = false, row = 0;

  function light() {
    rows.forEach((r, i) => r.classList.toggle("nfs-row--on", i === row));
  }

  function move(step) {
    frsmodloader.sound(step < 0 ? "up" : "down");
    row = Math.max(0, Math.min(rows.length - 1, row + step));
    light();
  }

  window.addEventListener("keydown", (e) => {
    if (!root.host.isConnected || !open) return;   // see below
    e.preventDefault();
    const k = e.keyCode;
    if (held.has(k)) return;          // held down: it already counted
    held.add(k);

    if (k === VK.UP) move(-1);
    else if (k === VK.DOWN) move(1);
    else if (k === VK.LEFT || k === VK.RIGHT) {
      frsmodloader.sound(k === VK.LEFT ? "valueLeft" : "valueRight");
      // ... change the value on `row`
    }
    else if (k === VK.ENTER || k === VK.SPACE) { frsmodloader.sound("confirm"); /* act */ }
    else if (k === VK.ESC || k === VK.BACK) close();
  });
  window.addEventListener("keyup", (e) => held.delete(e.keyCode));
  window.addEventListener("blur", () => held.clear());

  // Whatever takes the focus away, it comes back while the menu is up.
  menu.addEventListener("focusout", () => {
    if (open) setTimeout(() => open && menu.focus({ preventScroll: true }), 0);
  });

  frsmodloader.on("open", () => {
    open = true; row = 0; held.clear(); light();
    menu.focus({ preventScroll: true });
    frsmodloader.sound("open");
  });

  function close() {
    open = false;
    frsmodloader.sound("close");
    frsmodloader.send("close");       // main.js: speed.ui.capture(false)
  }
</script>
```

The `window` is the whole overlay's, shared by every page, and it outlives
yours: when your page is mounted again (a `panel_reload`, a reload while you
develop) the listeners the old copy put on `window` are still there. The
`frsmodloader.on` ones the shell takes away itself; for `window` and `document`
ones, `root.host.isConnected` is false in a copy that is gone - check it first
thing, or the old copy answers every key too.

Selection is the kit's own look: `.nfs-row--on` on the row, `.is-on` on a
button (`.nfs-btn`, `.nfs-arrow`) - the same classes the mouse's hover lights,
so both ways of driving the panel look alike. Where a screen has two places the
arrows can be (a list and the thing it selected, as the Mods menu has), keep a
"zone": Right or Enter goes into the second, Esc comes back out, and Esc again
closes.

## The palette

Every colour is a variable on `.nfs`, so a mod that wants the shapes without
the green only overrides a few:

```css
.nfs { --nfs-green: #ff9d3a; --nfs-edge: #ffcf94; --nfs-green-glow: rgba(255, 160, 60, .55); }
```

| | |
|---|---|
| `--nfs-green`, `--nfs-green-bright`, `--nfs-green-dim`, `--nfs-green-glow` | the identity |
| `--nfs-edge`, `--nfs-edge-soft` | the panel outline |
| `--nfs-ink`, `--nfs-ink-soft`, `--nfs-ink-dim` | text |
| `--nfs-panel`, `--nfs-panel-head` | the window fills |
| `--nfs-row`, `--nfs-row-alt`, `--nfs-row-empty`, `--nfs-row-on`, `--nfs-gutter`, `--nfs-row-line` | the list |
| `--nfs-btn`, `--nfs-btn-edge` | buttons |
| `--nfs-sheet`, `--nfs-sheet-ink`, `--nfs-sheet-ink-soft`, `--nfs-option-on` | the options sheet |
| `--nfs-pulse-time`, `--nfs-press-time` | the selection's breath, an arrow's press |
| `--nfs-pin-*` | the six map-pin colours |
| `--nfs-size`, `--nfs-radius` | scale and corner |

## The game's own textures

Underground 2's menus are textured, not drawn: the pill button is
`UI_PC_GENERIC_BUTTON`, its highlight `UI_PC_GENERIC_BUTTON_GLOW`, and so on.
FRSModLoader reads the game's texture packs (`FRONTEND\FRONTB.LZC`,
`GLOBAL\GLOBALB.BUN`, `GLOBAL\INGAMECOMMON.BUN`) from the game folder when a
page first asks, and serves any texture in them to Chromium as a PNG:

```
http://nfsu2.tex/UI_PC_GENERIC_BUTTON              by name
http://nfsu2.tex/UI_PC_GENERIC_BUTTON_GLOW?tint=b9e75b
http://nfsu2.tex/U2_MENU_ARROW?flip=h              h, v or hv
http://nfsu2.tex/U2_MENU_ROUNDEDCORNER?mirror      a 16x16 corner made a whole box
http://nfsu2.tex/U2_MENU_BOX?stack=3               three layers of it, as one
http://nfsu2.tex/0x26A17126                        by hash
http://nfsu2.tex/index.json                        every texture there is
```

Nothing is extracted to disk and nothing is in the repository or the release:
the pixels are the player's own copy of the game, so a texture mod that
replaces those packs shows up in the UI too.

- **Names** are the game's. The packs store them cut at 23 characters but keep
  the hash of the full one, so `UI_PC_GENERIC_BUTTON_GLOW` works;
  `index.json` lists the stored form, which works as well.
- **`mirror`** takes a quarter - the game draws its rounded boxes from one
  16x16 corner, rotated four times - and returns the four together, ready to
  nine-slice with `border-image`.
- **`stack`** is N copies of the texture drawn over itself: same colour,
  alpha `1 - (1 - a)^N`. The menus get their black by layering one 60% box.
- **`tint`** multiplies the texture by a colour, `RRGGBB` or `RRGGBBAA` — what
  FEng does to every quad it draws. The white pieces are meant to be tinted.
- It is plain `http`, so it works from `url()`, `<img>`, `fetch` and canvas,
  and it answers with `Access-Control-Allow-Origin: *`, which `mask-image`
  needs from a `file://` page.

The panel's fill is the game's too: the World Map stacks three layers of
`U2_MENU_ROUNDEDCORNER`/`U2_MENU_BOX` (black at 60%, pixel corners), and so
does `.nfs-panel`. The palette itself - the edge, the rows, the text, the
check - was measured off a screenshot of that screen. Unlike a border, a fill cannot fall back by itself, so it
waits for the shell: when the texture server answers, the shell puts
`data-nfs-tex` on the page and on every mod's host, and only then does the flat
fill step aside. A page outside the shell can set that attribute itself.

The kit keeps its textured pieces in variables, so a palette that moves away
from green re-tints them by overriding those:

```css
.nfs {
  --nfs-tex-btn-on:   url("http://nfsu2.tex/UI_PC_GENERIC_BUTTON?tint=ff9d3a");
  --nfs-tex-btn-glow: url("http://nfsu2.tex/UI_PC_GENERIC_BUTTON_GLOW?tint=ff9d3a");
  --nfs-tex-ring-on:  url("http://nfsu2.tex/U2_MENU_ARROW_HIGHLIGHT?tint=ff9d3a");
  --nfs-tex-tile-on:  url("http://nfsu2.tex/U2_ICON_HIGHLIGHT?tint=ff9d3a");
}
```

## The font

The game's face is not a font that ships with Windows, and this does not bundle
one — a mod that runs offline inside a 2004 executable should not wait on a web
font. The stack is Segoe UI / Arial Narrow with the italic that carries most of
the resemblance. If you have the real thing as a `.woff2` next to your page, an
`@font-face` in your own `<style>` and one line is all it takes:

```css
.nfs { font-family: "Your Font", "Segoe UI", sans-serif; }
```

## Native mods too

Nothing here is JavaScript, so an `.asi` mod using the
[host API](NATIVE_PLUGINS.md) gets it the same way: the `<link>` goes in the
HTML file you hand to `panel_open`, and the rest is the classes above.
