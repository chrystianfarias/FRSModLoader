# Contributing

Pull requests are welcome — mods, SDK surface, bug fixes, documentation. They
are merged after review and approval by the maintainer (Chrystian Farias).

By opening a pull request you agree that your contribution is distributed under
the project's terms of use in [LICENSE](LICENSE).

## Before you start

If what you want to write is a mod, [docs/MODDING.md](docs/MODDING.md) is the
guide: the manifest, the two halves, the SDK, and what not to do on the game
thread.

For anything larger than a fix — a new hook, a change to the `speed.*` API, a
new subsystem — open an issue first and describe what you want to do. It saves
you from writing something that takes a different direction than the platform.

## Setting up

Visual Studio 2022 with the C++ toolset (x86). Everything else comes down on
its own: CEF is downloaded on first build (~147 MB), QuickJS through
FetchContent.

```
build.bat                    build and install into the game folder
build.bat nomod              build only
.\install.ps1 -ModsOnly      refresh mods and UI, game can stay open
.\run.ps1 -Seconds 40        launch the game and return the log
```

Point the installer somewhere else with `.\install.ps1 -Game "D:\Games\NFSU2"`.

## What a good pull request looks like

- **One subject per PR.** A mod, a fix, a documented address — not three.
- **Say how you tested it.** Which game version, which scene, what you saw.
  `FRSModLoader.log` excerpts help; a screenshot helps more for UI.
- **Game addresses come with evidence.** A new address in `src/game` or in a
  mod needs a note in `NOTES.md` saying how you found it and what it holds.
  An address without provenance is a crash waiting for someone else.
- **Match the surrounding code.** Same naming, same comment density, same
  layering (`core`, `game`, `js`, `ui`, `bridge`). No reformatting of files you
  did not otherwise touch.
- **Do not commit build output.** `build/`, `third_party/cef/` and the
  reference sources in `refs/` are ignored on purpose.
- **Mods stay self-contained.** A mod is its own folder under `mods/`, with
  `mod.json`, its `main.js` and its `ui/`. It must not require changes to the
  loader to work.

## Reporting a bug

Include the game version and executable size (this targets `SPEED2.EXE` v1.2
NTSC, 4,800,512 bytes), your `FRSModLoader.ini`, the relevant part of
`scripts\FRSModLoader.log`, and which other `.asi` mods you have installed —
most of the interesting failures are a conflict in the main loop.
