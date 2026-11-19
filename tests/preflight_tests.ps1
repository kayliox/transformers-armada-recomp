# preflight_tests.ps1 - tools\preflight.ps1 (the checks before the setup's long steps) against simulated PCs: low disk
# space on the install's drive or on Windows' drive, a cloud-synced folder, a network share, letters outside plain
# English, a path too long without Windows' long paths, a file that is not a disc image, and a healthy PC. Nothing is
# installed or changed: the drive and the folder lookups are stood in for.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\preflight_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}
. (Join-Path $Root "tools\install_layout.ps1")
. (Join-Path $Root "tools\preflight.ps1")
function Levels($f, [string]$level) { @($f | Where-Object { $_.Level -eq $level }) }
function Has($f, [string]$level, [string]$text) { [bool](Levels $f $level | Where-Object { $_.Text -like "*$text*" }) }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("tfa_preflight_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
$saveOneDrive = $env:OneDrive
try {
    # Plenty of space everywhere (the stand-in), the folder in a plain place.
    $script:SimFree = @{}
    function Get-DriveFreeGB([string]$Path) {
        $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
        $free = if ($script:SimFree.ContainsKey($root)) { $script:SimFree[$root] } else { 500 }
        [pscustomobject]@{ Root = $root; FreeGB = $free; Format = "NTFS"; Type = "Fixed" }
    }
    $env:OneDrive = $null
    $f = Invoke-Preflight -Port $tmp -Python ""
    Check ((Levels $f "stop").Count -eq 0) "a healthy PC: nothing stops"

    $script:SimFree = @{ ([IO.Path]::GetPathRoot($tmp)) = 3 }
    $f = Invoke-Preflight -Port $tmp -Python ""
    Check (Has $f "stop" "Not enough free space") "3 GB free on the install's drive: stops, says how much is needed"
    $f = Invoke-Preflight -Port $tmp -Python "" -Update
    Check (-not (Has $f "stop" "for the update")) "3 GB free before an update rebuild (1.5 needed): goes on"
    $script:SimFree = @{ ([IO.Path]::GetPathRoot($tmp)) = 1 }
    $f = Invoke-Preflight -Port $tmp -Python "" -Update
    Check (Has $f "stop" "for the update") "1 GB free before an update rebuild: stops"
    $script:SimFree = @{ ([IO.Path]::GetPathRoot($tmp)) = 9 }
    $f = Invoke-Preflight -Port $tmp -Python "" -HdQuality fast
    Check (-not (Has $f "stop" "Not enough")) "9 GB free, HD fast, programs already there: goes on"
    $script:SimFree = @{}

    # (TODO 443) The Build Tools always go on Windows' drive: about 7 GB there even when the game goes elsewhere (a
    # player's install stopped with 0x80070070, the disk full, half way through Microsoft's installer). Windows' drive is
    # simulated as Y: (the game's folder stays where the test runs).
    $gameRoot = [IO.Path]::GetPathRoot($tmp)
    $saveSysDrive = $env:SystemDrive
    $env:SystemDrive = if ($gameRoot -eq "Y:\") { "X:" } else { "Y:" }
    $sysRoot = "$env:SystemDrive\"
    try {
        function Test-PreflightVsCpp { return $false }
        $script:SimFree = @{ $sysRoot = 5; $gameRoot = 500 }
        $f = Invoke-Preflight -Port $tmp -Python ""
        Check ((Has $f "stop" "Windows' drive $sysRoot") -and (Has $f "stop" "Build Tools go there even when the game is on another drive")) "no Build Tools, game on another drive, 5 GB free on Windows' drive: stops, names that drive"
        $script:SimFree = @{ $sysRoot = 9; $gameRoot = 500 }
        $f = Invoke-Preflight -Port $tmp -Python ""
        Check ((Levels $f "stop").Count -eq 0) "no Build Tools, game on another drive, 9 GB free on Windows' drive: goes on"
        function Test-PreflightVsCpp { return $true }
        $script:SimFree = @{ $sysRoot = 5; $gameRoot = 500 }
        $f = Invoke-Preflight -Port $tmp -Python ""
        Check (-not (Has $f "stop" "Windows' drive")) "Build Tools already there, game on another drive, 5 GB free on Windows' drive: goes on"
    } finally { $env:SystemDrive = $saveSysDrive }
    $env:SystemDrive = $gameRoot.TrimEnd('\')
    try {
        function Test-PreflightVsCpp { return $false }
        $script:SimFree = @{ $gameRoot = 12 }
        $f = Invoke-Preflight -Port $tmp -Python "" -HdQuality fast
        Check (Has $f "stop" "7 GB of it for Microsoft's C++ Build Tools") "no Build Tools, game on Windows' drive, 12 GB free: stops, says 7 GB of it is the Build Tools"
    } finally { $env:SystemDrive = $saveSysDrive }
    Remove-Item Function:\Test-PreflightVsCpp
    . (Join-Path $Root "tools\preflight.ps1")
    function Get-DriveFreeGB([string]$Path) {
        $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
        $free = if ($script:SimFree.ContainsKey($root)) { $script:SimFree[$root] } else { 500 }
        [pscustomobject]@{ Root = $root; FreeGB = $free; Format = "NTFS"; Type = "Fixed" }
    }
    $script:SimFree = @{}

    # (TODO 441) an external drive and Controlled Folder Access: said, the setup goes on
    function Get-PreflightBusType([string]$Path) { return "USB" }
    function Get-PreflightCfa { return 1 }
    $f = Invoke-Preflight -Port $tmp -Python ""
    Check ((Has $f "warn" "external drive") -and (Has $f "warn" "Controlled Folder Access") -and (Levels $f "stop").Count -eq 0) "an external (USB) drive and Controlled Folder Access on: both said, nothing stops"
    function Get-PreflightBusType([string]$Path) { return "NVMe" }
    function Get-PreflightCfa { return 0 }
    $f = Invoke-Preflight -Port $tmp -Python ""
    Check (-not (Has $f "warn" "external drive") -and -not (Has $f "warn" "Controlled Folder Access")) "the PC's own disk, Controlled Folder Access off: nothing said"

    $cloud = Join-Path $tmp "OneDrive - Contoso"
    $inCloud = Join-Path $cloud "Desktop\TFA\transformers-armada-recomp"
    New-Item -ItemType Directory -Force $inCloud | Out-Null
    $env:OneDrive = $cloud
    $f = Invoke-Preflight -Port $inCloud -Python ""
    Check (Has $f "warn" "cloud folder") "a folder inside OneDrive: said, with a folder outside it to use"
    $f = Invoke-Preflight -Port $tmp -Python ""
    Check (-not (Has $f "warn" "cloud folder")) "a folder beside OneDrive (not inside): nothing said"
    $env:OneDrive = $null

    $f = Invoke-Preflight -Port "\\nas\games\TFA" -Python ""
    Check (Has $f "stop" "network share") "a network share: stops"

    $f = Invoke-Preflight -Port (Join-Path $tmp ("Jogos\Transforma" + [char]0x00E7 + [char]0x00E3 + "o")) -Python ""
    Check (Has $f "warn" "outside plain English") "letters outside plain English: said"

    $long = Join-Path $tmp ("x" * 120)
    $f = Invoke-Preflight -Port $long -Python ""
    $lp = $false
    try { $lp = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -ErrorAction Stop).LongPathsEnabled -eq 1 } catch {}
    if ($lp) { Check (-not (Has $f "warn" "path is long")) "a long path with Windows' long paths on: nothing said" }
    else { Check (Has $f "warn" "path is long") "a long path with Windows' long paths off: said" }

    $notIso = Join-Path $tmp "not_a_disc.iso"
    Set-Content -LiteralPath $notIso -Value ("x" * 100000)
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) {
        $f = Invoke-Preflight -Port $Root -Iso $notIso -Python $py
        Check (Has $f "stop" "cannot be read as a disc image") "a file that is not a disc image: stops before anything is installed"
    } else { Write-Host "  skip  the disc check (no python on PATH)" }
    $f = Invoke-Preflight -Port $Root -Iso $notIso -Python ""
    Check (-not (Has $f "stop" "disc image")) "no Python yet (a fresh PC): the disc check waits for the build"

    Check ((Write-PreflightReport @([pscustomobject]@{ Level = "warn"; Text = "a" })) -eq $true) "the report: a warning alone goes on"
    Check ((Write-PreflightReport @([pscustomobject]@{ Level = "stop"; Text = "b" })) -eq $false) "the report: a stop stops"

    # Dot-sourced by setup.ps1: it must not change the caller's variables (setup.ps1's own $Iso, $Port).
    $Iso = "keep-me"; $Port = "keep-me-too"
    . (Join-Path $Root "tools\preflight.ps1")
    Check ($Iso -eq "keep-me" -and $Port -eq "keep-me-too") "dot-sourcing it leaves the caller's own variables alone"

    # The report a stopped setup writes (tools\setup_report.ps1), and setup.ps1 itself writing it when it stops.
    . (Join-Path $Root "tools\setup_report.ps1")
    $env:TF_REPORT_DIR = Join-Path $tmp "reports"
    New-Item -ItemType Directory -Force $env:TF_REPORT_DIR | Out-Null
    $rep = New-SetupReport -Port $Root -What "setup" -Message "test: stopped at $env:USERPROFILE\x on $env:COMPUTERNAME"
    $text = if ($rep) { Get-Content -LiteralPath $rep -Raw } else { "" }
    Check ($text -like "*where it stopped*" -and $text -like "*Visual Studio*" -and $text -like "*the build's state*") "the report: the error, the tools and the build's state"
    Check ($text -notmatch [regex]::Escape($env:USERNAME) -and $text -notmatch [regex]::Escape($env:COMPUTERNAME) -and $text -like "*<home>\x on <pc>*") "the report: no user name, user folder or PC name in it"
    Remove-Item -LiteralPath $rep
    $o = powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root "setup.ps1") -Iso (Join-Path $tmp "no_such_disc.iso") 2>&1 | Out-String
    $made = @(Get-ChildItem -LiteralPath $env:TF_REPORT_DIR -Filter "tfa-report-*.txt")
    Check ($made.Count -eq 1 -and (($o -split "`n") | Where-Object { $_ -like "*report to attach*" } | Where-Object { $_ -notmatch [regex]::Escape($env:USERPROFILE) }) -and (Get-Content -LiteralPath $made[0].FullName -Raw) -like "*no such file*") "setup.ps1 stopping writes the report and says where"
    $env:TF_REPORT_DIR = $null
}
finally {
    $env:OneDrive = $saveOneDrive
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "`n$failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall checks passed" -ForegroundColor Green
exit 0
