# Downloads and extracts the CEF binary distribution (windows32, minimal) into
# third_party\cef. Run it once; after that the build never touches the network.
#
#   .\tools\fetch_cef.ps1            # the version pinned below
#   .\tools\fetch_cef.ps1 -Force     # download again even if it is present

param(
    [string]$Version = "152.0.6+g708dc14+chromium-152.0.7977.83",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "third_party\cef"
$stamp = Join-Path $dest ".version"

if ((Test-Path $stamp) -and -not $Force) {
    $have = (Get-Content $stamp -Raw).Trim()
    if ($have -eq $Version) { "[ok] CEF $Version is already in third_party\cef"; exit 0 }
    "[..] installed version ($have) differs from the requested one; downloading again"
}

# The file name travels url-encoded; '+' has to become %2B.
$name = "cef_binary_${Version}_windows32_minimal.tar.bz2"
$url = "https://cef-builds.spotifycdn.com/" + $name.Replace("+", "%2B")
$tmp = Join-Path $env:TEMP "frsmodloader_cef"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$archive = Join-Path $tmp $name

if (-not (Test-Path $archive) -or $Force) {
    "[..] downloading $name (~147 MB)"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
    "[ok] downloaded in $([int]$sw.Elapsed.TotalSeconds)s"
}

"[..] extracting"
$stage = Join-Path $tmp "stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
# The Windows tar (bsdtar) reads .tar.bz2 directly.
tar -xf $archive -C $stage
if ($LASTEXITCODE -ne 0) { throw "tar failed" }

$inner = Get-ChildItem $stage -Directory | Select-Object -First 1
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
Move-Item $inner.FullName $dest

Set-Content -Path $stamp -Value $Version -Encoding utf8
Remove-Item -Recurse -Force $stage
"[ok] CEF $Version in $dest"
