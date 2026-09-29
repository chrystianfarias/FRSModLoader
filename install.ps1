# Installs FRSModLoader into the game's scripts\ folder.
#
# What ends up in the game:
#
#   scripts\FRSModLoader.asi       the injected loader
#   scripts\FRSModLoader.ini       configuration
#   scripts\FRSModLoader\          Chromium runtime and helper
#   scripts\FRSModLoader\ui\       shell.html and the generated mods.json
#   scripts\FRSModLoader\mods\     one subdirectory per mod
#
#   .\install.ps1                  install everything
#   .\install.ps1 -ModsOnly        mods and UI only (leaves Chromium alone)

param(
    [string]$Game = "F:\Games\NFSU2",
    [switch]$ModsOnly
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build\Release"
$cef = Join-Path $root "third_party\cef"
$dest = Join-Path $Game "scripts"
$runtime = Join-Path $dest "FRSModLoader"

if (-not (Test-Path $Game)) { throw "game not found at $Game" }
New-Item -ItemType Directory -Force -Path $runtime | Out-Null

# ---- migration from SpeedLoader, the loader's former name ----------------
# Left in place, the old .asi would hook the game alongside the new one. The
# user's ini and each mod's saved data come along under the new name; the
# rest (Chromium, helper, logs) is reinstalled or regenerated.
$oldRuntime = Join-Path $dest "SpeedLoader"
$oldAsi = Join-Path $dest "SpeedLoader.asi"
if ((Test-Path $oldAsi) -or (Test-Path $oldRuntime)) {
    if (Get-Process SPEED2 -ErrorAction SilentlyContinue) {
        throw "the game is running the old SpeedLoader.asi: close SPEED2.EXE " +
              "so it can be replaced by FRSModLoader"
    }

    $oldIni = Join-Path $dest "SpeedLoader.ini"
    $newIni = Join-Path $dest "FRSModLoader.ini"
    if ((Test-Path $oldIni) -and -not (Test-Path $newIni)) {
        # Its paths (Dir = SpeedLoader\mods) move with it. ANSI, which is what
        # GetPrivateProfileString reads.
        (Get-Content $oldIni -Raw) -replace 'SpeedLoader', 'FRSModLoader' |
            Set-Content $newIni -NoNewline -Encoding Default
    }

    $oldData = Join-Path $oldRuntime "data"
    if ((Test-Path $oldData) -and -not (Test-Path (Join-Path $runtime "data"))) {
        Move-Item $oldData $runtime
    }

    foreach ($stale in $oldAsi, $oldRuntime, $oldIni,
                       (Join-Path $dest "SpeedLoader.log"),
                       (Join-Path $dest "SpeedLoaderCef.log")) {
        if (Test-Path $stale) { Remove-Item $stale -Recurse -Force }
    }
    "[ok] migrated SpeedLoader to FRSModLoader"
}

# ---- mods and UI: what changes on every iteration ------------------------
# Delete before copying: Copy-Item -Recurse only merges, so a mod removed from
# the source tree would stay installed in the game, haunting the UI.
# Every mod goes through modcheck before reaching the game: a function that is
# called but does not exist only shows up when someone clicks the button that
# goes through it, and by then it has already cost a race. Without node
# installed, it just moves on.
if ((Get-Command node -ErrorAction SilentlyContinue) -and
    (Test-Path (Join-Path $root "mods"))) {
    $check = Join-Path $root "tools\modcheck.js"
    foreach ($mod in Get-ChildItem (Join-Path $root "mods") -Directory) {
        $main = Join-Path $mod.FullName "main.js"
        if (-not (Test-Path $main)) { continue }
        $output = & node $check $main 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "$($mod.Name): $output"
        }
    }
}

foreach ($folder in "ui", "mods") {
    $source = Join-Path $root $folder
    # mods\ is not in the repository: a fresh clone has the platform and no
    # mods, and that installs perfectly well.
    if (-not (Test-Path $source)) { continue }

    $target = Join-Path $runtime $folder
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Copy-Item $source $runtime -Recurse -Force
}

$ini = Join-Path $dest "FRSModLoader.ini"
if (-not (Test-Path $ini)) {
    # The installed ini is never overwritten: it belongs to the user, not to
    # the build.
    Copy-Item (Join-Path $root "FRSModLoader.ini") $ini
}

if ($ModsOnly) { "[ok] mods and UI updated"; exit 0 }

# ---- binaries ------------------------------------------------------------
if (-not (Test-Path (Join-Path $build "FRSModLoader.asi"))) {
    throw "FRSModLoader.asi not found. Run build.bat first."
}

if (Get-Process SPEED2 -ErrorAction SilentlyContinue) {
    throw "the game is running: close SPEED2.EXE before installing the .asi " +
          "(or use -ModsOnly, which does not touch the binaries)"
}

Copy-Item (Join-Path $build "FRSModLoader.asi") $dest -Force
Copy-Item (Join-Path $build "FRSModLoaderHelper.exe") $runtime -Force

# The native plugin: the .asi goes next to the loader and its page into a
# folder named after it, which is where panel_open("SpawnCar\ui.html") looks.
if (Test-Path (Join-Path $build "SpawnCar.asi")) {
    Copy-Item (Join-Path $build "SpawnCar.asi") $dest -Force
    $spawnDir = Join-Path $dest "SpawnCar"
    New-Item -ItemType Directory -Force -Path $spawnDir | Out-Null
    Copy-Item (Join-Path $root "plugins\spawn-car\ui.html") $spawnDir -Force
}

# ---- Chromium runtime ----------------------------------------------------
if (-not (Test-Path (Join-Path $cef "Release\libcef.dll"))) {
    throw "CEF is missing. Run tools\fetch_cef.ps1 first."
}

# Binaries (dll + .bin) and resources (.pak, icudtl.dat, locales\).
Get-ChildItem (Join-Path $cef "Release") -File |
    Where-Object { $_.Extension -in ".dll", ".bin", ".json" } |
    ForEach-Object { Copy-Item $_.FullName $runtime -Force }

Copy-Item (Join-Path $cef "Resources\*") $runtime -Recurse -Force

"[ok] FRSModLoader installed in $dest"
