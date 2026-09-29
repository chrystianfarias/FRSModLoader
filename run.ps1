# Launches the game with focus, waits, and returns FRSModLoader's log.
#
#   .\run.ps1                 # 30 s, filtered log
#   .\run.ps1 -Seconds 60
#   .\run.ps1 -Full           # the whole log, CEF's included
#   .\run.ps1 -KeepOpen       # do not kill the game at the end

param(
    [int]$Seconds = 30,
    [string]$Game = "F:\Games\NFSU2",
    [switch]$Full,
    [switch]$KeepOpen
)

# The game runs with scripts\ as its working directory (that is where the .asi
# loader puts it), so that is where the log lands.
$log = Join-Path $Game "scripts\FRSModLoader.log"
$cefLog = Join-Path $Game "scripts\FRSModLoaderCef.log"
Remove-Item $log, $cefLog -ErrorAction SilentlyContinue

Add-Type -Namespace SL -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
'@ -ErrorAction SilentlyContinue

$proc = Start-Process -FilePath (Join-Path $Game "SPEED2.EXE") -WorkingDirectory $Game -PassThru

# Without focus the frontend freezes; same care as in Pops.
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 200
    $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if (-not $p) { break }
    $hwnd = $p.MainWindowHandle
}
for ($i = 0; $i -lt 8; $i++) { [void][SL.Win]::SetForegroundWindow($hwnd); Start-Sleep -Milliseconds 300 }

Start-Sleep -Seconds $Seconds

$exitedOnItsOwn = $proc.HasExited
if (-not $exitedOnItsOwn -and -not $KeepOpen) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 800

"--- game exited on its own: $exitedOnItsOwn ---"
if (-not (Test-Path $log)) { "no FRSModLoader.log"; exit }

if ($Full) {
    Get-Content $log
    if (Test-Path $cefLog) { "--- CEF ---"; Get-Content $cefLog }
} else {
    Get-Content $log | Select-String -Pattern 'core|gfx|cef|js|in\]'
}
