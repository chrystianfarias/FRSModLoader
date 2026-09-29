# Stages a distributable build of FRSModLoader into release\.
#
# The release folder is local - it is in .gitignore and never committed.
#
#   .\release.ps1                     build if needed, stage and zip
#   .\release.ps1 -Version 1.2.1      name the package (default: src\core\Version.h)
#   .\release.ps1 -NoZip              leave the folder, skip the .zip
#   .\release.ps1 -Rebuild            build first, even if binaries exist
#   .\release.ps1 -NoAsiLoader        package without the .asi loader
#
# What the package looks like. Its contents go into the game folder, next to
# SPEED2.EXE, exactly as they are:
#
#   FRSModLoader-<version>\
#     INSTALL.txt
#     LICENSE.txt                 ours, plus CEF's and the loader's
#     dinput8.dll                 Ultimate ASI Loader - what loads the .asi
#     scripts\FRSModLoader.asi
#     scripts\FRSModLoader.ini
#     scripts\FRSModLoader\       Chromium runtime, helper, ui\ and an empty mods\
#
# The package is the platform and nothing else: no mod ships with the loader.
# Mods are released on their own and dropped into scripts\FRSModLoader\mods\.

param(
    [string]$Version,
    [switch]$NoZip,
    [switch]$Rebuild,
    [switch]$NoAsiLoader
)

$ErrorActionPreference = "Stop"
$root   = $PSScriptRoot

# The version the .asi reports is the one the package is named after.
if (-not $Version) {
    $header = Get-Content (Join-Path $root "src\core\Version.h") -Raw
    if ($header -notmatch 'FRSMODLOADER_VERSION\s+"([^"]+)"') {
        throw "no FRSMODLOADER_VERSION in src\core\Version.h"
    }
    $Version = $Matches[1]
}
$build  = Join-Path $root "build\Release"
$cef    = Join-Path $root "third_party\cef"
$loader = Join-Path $root "third_party\asi-loader"

# ---- build ---------------------------------------------------------------
$asi = Join-Path $build "FRSModLoader.asi"
if ($Rebuild -or -not (Test-Path $asi)) {
    "[..] building"
    & (Join-Path $root "build.bat") nomod
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
if (-not (Test-Path $asi)) { throw "FRSModLoader.asi not found. Run build.bat first." }
if (-not (Test-Path (Join-Path $cef "Release\libcef.dll"))) {
    throw "CEF is missing. Run tools\fetch_cef.ps1 first."
}

# The .asi loader is what loads FRSModLoader.asi in the first place. Shipping it
# is the convention for NFSU2 mods - ExtraOptions does the same - and it saves
# the player a second download. -NoAsiLoader is for whoever already has one.
if (-not $NoAsiLoader -and -not (Test-Path (Join-Path $loader "dinput8.dll"))) {
    & (Join-Path $root "tools\fetch_asi_loader.ps1")
    if ($LASTEXITCODE -ne 0) { throw "could not fetch the ASI loader" }
}

# ---- a clean staging tree ------------------------------------------------
$name    = "FRSModLoader-$Version"
$stage   = Join-Path $root "release\$name"
$scripts = Join-Path $stage "scripts"
$runtime = Join-Path $scripts "FRSModLoader"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $runtime | Out-Null

# ---- binaries ------------------------------------------------------------
Copy-Item $asi $scripts -Force
Copy-Item (Join-Path $build "FRSModLoaderHelper.exe") $runtime -Force
Copy-Item (Join-Path $root "FRSModLoader.ini") $scripts -Force

if (-not $NoAsiLoader) {
    Copy-Item (Join-Path $loader "dinput8.dll") $stage -Force
}

# ---- Chromium runtime ----------------------------------------------------
# Binaries (dll + .bin + .json) and resources (.pak, icudtl.dat, locales\).
Get-ChildItem (Join-Path $cef "Release") -File |
    Where-Object { $_.Extension -in ".dll", ".bin", ".json" } |
    ForEach-Object { Copy-Item $_.FullName $runtime -Force }

Copy-Item (Join-Path $cef "Resources\*") $runtime -Recurse -Force

# ---- UI shell, and a place for mods ---------------------------------------
# ui\ as git has it: ui\fonts holds a licensed font installed on this machine
# only, which must never travel.
$uiOut = Join-Path $runtime "ui"
New-Item -ItemType Directory -Force -Path $uiOut | Out-Null
git -C $root ls-files ui | ForEach-Object {
    $dest = Join-Path $runtime $_
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Copy-Item (Join-Path $root $_) $dest -Force
}

# Empty: the loader ships no mod.
New-Item -ItemType Directory -Force -Path (Join-Path $runtime "mods") | Out-Null

# ---- paperwork -----------------------------------------------------------
# One LICENSE.txt for the whole package: ours, then the components we ship.
# Both of them are permissive, and both require their notice to travel along.
$parts = @(
    "FRSModLoader - LICENSE",
    "",
    "This package contains FRSModLoader, and third-party components that keep",
    "their own licenses. Each one is reproduced in full below.",
    "",
    ("=" * 76),
    "1. FRSModLoader",
    ("=" * 76),
    "",
    (Get-Content (Join-Path $root "LICENSE") -Raw).TrimEnd(),
    "",
    ("=" * 76),
    "2. Chromium Embedded Framework (CEF) and Chromium",
    ("=" * 76),
    "",
    (Get-Content (Join-Path $cef "LICENSE.txt") -Raw).TrimEnd()
)

# QuickJS is linked into FRSModLoader.asi, so its notice travels too.
$quickjs = Join-Path $root "build\_deps\quickjs-src\LICENSE"
if (Test-Path $quickjs) {
    $parts += @(
        "",
        ("=" * 76),
        "3. QuickJS, by Fabrice Bellard and Charlie Gordon (linked into the .asi)",
        ("=" * 76),
        "",
        (Get-Content $quickjs -Raw).TrimEnd()
    )
}

if (-not $NoAsiLoader) {
    $parts += @(
        "",
        ("=" * 76),
        "4. Ultimate ASI Loader (dinput8.dll), by ThirteenAG",
        ("=" * 76),
        "   https://github.com/ThirteenAG/Ultimate-ASI-Loader",
        "",
        (Get-Content (Join-Path $loader "LICENSE.txt") -Raw).TrimEnd()
    )
}

($parts -join "`r`n") | Set-Content (Join-Path $stage "LICENSE.txt") -Encoding utf8

$loaderLines = if ($NoAsiLoader) {
@"
  You need an .asi loader already installed - this package does not bring one.
"@
} else {
@"
  dinput8.dll is Ultimate ASI Loader, by ThirteenAG. It is what loads the
  .asi at startup. If you already have an .asi loader (dinput8.dll, dsound.dll,
  vorbisFile.dll, ...) in the game folder, keep yours and skip this file.
"@
}

@"
FRSModLoader $Version
A modding platform for NFS Underground 2 (SPEED2.EXE v1.2 NTSC, 4,800,512 bytes).

INSTALL

  1. Close the game.
  2. Copy everything in this folder into your NFSU2 folder, next to SPEED2.EXE,
     and let "scripts" merge with the one already there.
  3. Start the game.

$loaderLines

  Upgrading: your scripts\FRSModLoader.ini is yours - keep it, and compare it
  with the one in this package if a new key shows up.

MODS

  None come with the loader. A mod is a folder: drop it into
  scripts\FRSModLoader\mods\ and start the game. Options > Mods lists every
  mod installed, switches each on or off, and holds its settings.

KEYS

  F1   hands the keyboard to the UI, and back to the game
       (configurable in FRSModLoader.ini). The mouse needs no key: a click
       on a mod's panel is the panel's.
  /    opens the in-game console

WHAT IS IN HERE

  dinput8.dll                    the .asi loader (Ultimate ASI Loader)
  scripts\FRSModLoader.asi       FRSModLoader itself
  scripts\FRSModLoader.ini       configuration
  scripts\FRSModLoader\          Chromium runtime and the helper process
  scripts\FRSModLoader\ui\       the shell that mounts each mod's UI
  scripts\FRSModLoader\mods\     where mods go (empty)

TROUBLE

  scripts\FRSModLoader.log is the first place to look; Chromium's own log is
  FRSModLoaderCef.log next to it. Most surprises are a conflict with another
  .asi in the main loop - say which ones you have when reporting a problem.

LICENSE

  FRSModLoader by Chrystian Farias
  https://github.com/chrystianfarias/SpeedLoader

  CC BY-NC 4.0 - Copyright (c) 2025 Chrystian Farias. See LICENSE.
  Redistribution is fine with credit, and not for commercial purposes.
  CEF/Chromium and Ultimate ASI Loader keep their own licenses. All of them
  are in LICENSE.txt, in full.

  Not affiliated with Electronic Arts. No game file is distributed here.
"@ | Set-Content (Join-Path $stage "INSTALL.txt") -Encoding utf8

# ---- zip -----------------------------------------------------------------
$size = "{0:N1} MB" -f ((Get-ChildItem $stage -Recurse -File |
    Measure-Object Length -Sum).Sum / 1MB)

if (-not $NoZip) {
    $zip = Join-Path $root "release\$name.zip"
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
    "[ok] release\$name.zip ($size unpacked)"
} else {
    "[ok] release\$name ($size)"
}
