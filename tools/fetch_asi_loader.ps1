# Downloads Ultimate ASI Loader (x86) into third_party\asi-loader, so that
# release.ps1 can ship it next to the game executable. Run it once.
#
#   .\tools\fetch_asi_loader.ps1            # the version pinned below
#   .\tools\fetch_asi_loader.ps1 -Force     # download again even if present
#
# The loader is ThirteenAG's, MIT licensed; its license comes down with it and
# travels inside every package we build.

param(
    [string]$Version = "v9.7.4",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\asi-loader"
$stamp = Join-Path $dest ".version"

if ((Test-Path $stamp) -and -not $Force) {
    $have = (Get-Content $stamp -Raw).Trim()
    if ($have -eq $Version) { "[ok] ASI loader $Version is already in third_party\asi-loader"; exit 0 }
    "[..] installed version ($have) differs from the requested one; downloading again"
}

$base = "https://github.com/ThirteenAG/Ultimate-ASI-Loader"
$tmp = Join-Path $env:TEMP "frsmodloader_asiloader"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$archive = Join-Path $tmp "Ultimate-ASI-Loader-NoPDB.zip"

"[..] downloading Ultimate ASI Loader $Version (x86)"
Invoke-WebRequest -Uri "$base/releases/download/$Version/Ultimate-ASI-Loader-NoPDB.zip" `
    -OutFile $archive -UseBasicParsing

if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Expand-Archive -Path $archive -DestinationPath $dest -Force

$dll = Join-Path $dest "dinput8.dll"
if (-not (Test-Path $dll)) { throw "dinput8.dll not found in the downloaded archive" }

Invoke-WebRequest -Uri "$base/raw/master/license" `
    -OutFile (Join-Path $dest "LICENSE.txt") -UseBasicParsing

Set-Content -Path $stamp -Value $Version -Encoding utf8
Remove-Item $archive -Force
"[ok] ASI loader $Version in $dest"
