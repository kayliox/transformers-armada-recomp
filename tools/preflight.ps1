<#
.SYNOPSIS
    (build robustness) The checks before any long step of the setup or an update: each problem found is said in plain
    words with what to do, before 30 minutes of installs and builds run into it.

.DESCRIPTION
    Invoke-Preflight returns a list of findings, each [pscustomobject]@{ Level = "stop" | "warn" | "info"; Text = "..." }:
      disk space   the install's drive (the build, the disc's files, the HD textures) and Windows' drive (the programs
                   the setup installs there), measured needs (docs\BUILD_ROBUSTNESS.md section 6);
      the folder   a cloud folder (OneDrive, Dropbox, Google Drive, iCloud: they sync and lock the build's files
                   while it runs), a network drive, a drive without file permissions (FAT32), letters outside plain
                   English in the path, a path too long for Windows' 260-character limit without long paths on;
      the disc     its size and the release's executable read straight from the image, its checksum compared with
                   the release's (a wrong disc or a bad copy found before anything is installed);
      Windows      Smart App Control on (it can refuse the game built on this PC), the antivirus programs, RAM.
    Write-PreflightReport prints them and writes each to the step log; it returns $false when one stops the setup.
    Standalone: powershell -File tools\preflight.ps1 [-Iso <disc image>] (prints the findings; exit 1 on a stop).

    Deps: Windows PowerShell 5.1+, Python (for the disc check: transformers\tools\extract_iso.py --check).
#>
# (no param() block: setup.ps1 and update.ps1 dot-source this file, and a param() would overwrite their own $Iso)

# The measured sizes (GB) the checks use. Build: out\ + work\ after a full build with the libraries as archives;
# disc: the disc's files; programs: Visual Studio's Build Tools with the C++ tools (6.1), Git, Python and pip's parts.
$script:PreflightNeeds = @{ build = 2.6; disc = 2.7; update = 1.5; programs = 7.0; temp = 1.0
                            hd = @{ fast = 1.2; balanced = 2.0; best = 3.0; none = 0.1 } }

function Get-DriveFreeGB([string]$Path) {
    try {
        $full = [IO.Path]::GetFullPath($Path)
        $root = [IO.Path]::GetPathRoot($full)
        $d = [IO.DriveInfo]::new($root)
        return [pscustomobject]@{ Root = $root; FreeGB = [math]::Round($d.AvailableFreeSpace / 1GB, 1); Format = $d.DriveFormat; Type = "$($d.DriveType)" }
    } catch { return $null }
}

# The cloud-sync folders this user has (their roots), from the environment and the usual places.
function Get-CloudFolders {
    $list = [Collections.Generic.List[string]]::new()
    foreach ($v in "OneDrive", "OneDriveConsumer", "OneDriveCommercial") {
        $p = [Environment]::GetEnvironmentVariable($v)
        if ($p) { $list.Add($p) }
    }
    foreach ($n in "Dropbox", "Google Drive", "My Drive", "iCloudDrive", "iCloud Drive", "Box", "MEGA") {
        $list.Add((Join-Path $env:USERPROFILE $n))
    }
    @($list | Where-Object { $_ } | Select-Object -Unique)
}

function Test-UnderFolder([string]$Path, [string]$Folder) {
    $p = ([IO.Path]::GetFullPath($Path)).TrimEnd('\') + '\'
    $f = ([IO.Path]::GetFullPath($Folder)).TrimEnd('\') + '\'
    return $p.StartsWith($f, [StringComparison]::OrdinalIgnoreCase)
}

# Whether a Visual Studio with the C++ tools is installed (vswhere). A function of its own so the tests can stand in for it.
function Test-PreflightVsCpp {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    return (Test-Path -LiteralPath $vswhere) -and [bool]("$(& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)".Trim())
}

# (TODO 441) How the drive holding a path is connected: "USB", "SD", "SATA", "NVMe", ... ("" when unknown). A function
# of its own so the tests can stand in for it.
function Get-PreflightBusType([string]$Path) {
    try {
        $full = [IO.Path]::GetFullPath($Path)
        if ($full -notmatch '^([A-Za-z]):') { return "" }
        $disk = Get-Partition -DriveLetter $Matches[1] -ErrorAction Stop | Get-Disk -ErrorAction Stop
        return "$($disk.BusType)"
    } catch { return "" }
}

# (TODO 441) Windows' Controlled Folder Access (ransomware protection): 1 on, 2 audit only, 0 off, -1 unknown.
function Get-PreflightCfa {
    try { return [int](Get-MpPreference -ErrorAction Stop).EnableControlledFolderAccess } catch { return -1 }
}

function Invoke-Preflight {
    param([string]$Iso, [string]$Port, [switch]$Update, [string]$HdQuality = "", [string]$Python = "python")
    $out = [Collections.Generic.List[object]]::new()
    function Add([string]$Level, [string]$Text) { $out.Add([pscustomobject]@{ Level = $Level; Text = $Text }) }
    $n = $script:PreflightNeeds
    $hd = if ($HdQuality -and $n.hd.ContainsKey($HdQuality)) { $n.hd[$HdQuality] } else { $n.hd.balanced }

    # --- the folder ---------------------------------------------------------------------------------------------------
    $full = [IO.Path]::GetFullPath($Port)
    foreach ($c in Get-CloudFolders) {
        if ((Test-Path -LiteralPath $c) -and (Test-UnderFolder $full $c)) {
            Add "warn" ("The game's folder is inside a cloud folder that is kept in sync ($c). The build writes thousands of files, " +
                "and the sync program reads and locks them while it runs: builds then fail at random or crawl. " +
                "Best: run the setup again with a folder outside it, for example $env:USERPROFILE\Games\TFA.")
            break
        }
    }
    try {
        $attr = [IO.File]::GetAttributes($full)
        # (RecallOnDataAccess 0x400000, Offline 0x1000: a cloud placeholder - files-on-demand)
        if (([int]$attr -band 0x401000) -ne 0) { Add "warn" "The game's folder is a cloud 'files on demand' folder: its files may be downloaded on every access. Choose a folder on the PC's own disk." }
    } catch {}
    if ($full.StartsWith('\\')) {
        Add "stop" "The game's folder is on a network share ($full). The build cannot run there: choose a folder on this PC's own disk."
    }
    $drive = Get-DriveFreeGB $full
    if ($drive -and $drive.Type -eq "Network") {
        Add "stop" "The game's folder is on a network drive ($($drive.Root)). The build cannot run there: choose a folder on this PC's own disk."
    }
    # (TODO 441) An external drive: the build writes and reads thousands of files, and the SDK scan its long log; a player's
    # setup on a drive like that sat at the scan for 50+ minutes (it takes 20 s on a PC's own disk).
    $bus = Get-PreflightBusType $full
    if ($bus -in "USB", "SD", "MMC", "1394" -or ($drive -and $drive.Type -eq "Removable")) {
        Add "warn" ("The game's folder is on an external drive ($($drive.Root), $(if ($bus) { $bus } else { 'removable' })). The build reads and writes thousands " +
            "of files there and can take hours instead of minutes. A folder on the PC's own disk (for example C:\Games\TFA) is much faster.")
    }
    if ((Get-PreflightCfa) -eq 1) {
        Add "warn" ("Windows' Controlled Folder Access (ransomware protection) is on. If a step stops with 'access denied' or sits still for many " +
            "minutes, allow the setup's programs (Windows Security > Virus & threat protection > Ransomware protection > Allow an app through " +
            "Controlled folder access) or turn it off during the install.")
    }
    if ($drive -and $drive.Format -eq "FAT32") {
        Add "warn" "The drive $($drive.Root) is FAT32: it cannot hold files over 4 GB and has no file permissions. An NTFS drive is better (most internal disks are)."
    }
    if ($full -match '[^\x20-\x7E]') {
        Add "warn" ("The game's folder has letters outside plain English in its path ($full). Most of the build copes, but if it stops " +
            "with a file that 'cannot be found' or 'cannot be opened', choose a folder like C:\Games\TFA.")
    }
    # (the deepest file the build makes is about 170 characters below the game's folder; Windows' limit is 260 without
    # its long-path setting)
    $long = $false
    try { $long = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -ErrorAction Stop).LongPathsEnabled -eq 1 } catch {}
    if (-not $long -and ($full.Length + 170) -gt 259) {
        Add "warn" ("The game's folder path is long ($($full.Length) characters) and Windows' long paths are off: some of the build's files would " +
            "pass Windows' 260-character limit. Choose a shorter folder (for example C:\Games\TFA).")
    }

    # --- disk space ---------------------------------------------------------------------------------------------------
    $gameDir = try { Get-GameDataDir $Port } catch { Join-Path $Port "game" }
    $haveDisc = (Test-Path -LiteralPath (Join-Path $gameDir "SLUS_206.68")) -or (Test-Path -LiteralPath (Join-Path $gameDir "SLES_523.88"))
    $needHere = if ($Update) { $n.update } else { $n.build + $n.temp + $hd + $(if ($haveDisc) { 0 } else { $n.disc }) }
    $sys = Get-DriveFreeGB "$env:SystemDrive\"
    $needSys = 0.0
    $vsSys = $false
    if (-not $Update) {
        $haveVs = Test-PreflightVsCpp
        # (TODO 443) Microsoft's C++ Build Tools: about 7 GB on Windows' drive whatever drive the game goes on (6.1 installed
        # + the installer's package cache; a player's install stopped half way with 0x80070070, the disk full)
        if (-not $haveVs) { $needSys += $n.programs; $vsSys = $true }
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $needSys += 0.4 }
        $needSys += 0.4  # (Python and pip's parts, the installers' own caches)
    }
    $vsHere = $false
    if ($drive -and $sys -and $drive.Root -eq $sys.Root) { $needHere += $needSys; $needSys = 0; $vsHere = $vsSys }
    if ($drive) {
        $what = if ($Update) { "the update" } else { "the install" }
        if ($drive.FreeGB -lt $needHere) {
            $vsPart = if ($vsHere) { " (about 7 GB of it for Microsoft's C++ Build Tools, which always go on Windows' drive)" } else { "" }
            Add "stop" ("Not enough free space on $($drive.Root) for ${what}: $($drive.FreeGB) GB free, about $([math]::Ceiling($needHere)) GB needed$vsPart. " +
                "Free some space (or choose a folder on another drive for the game), then run it again: finished steps are kept.")
        } elseif ($drive.FreeGB -lt $needHere * 1.3) {
            Add "warn" "Free space on $($drive.Root) is tight: $($drive.FreeGB) GB free, about $([math]::Ceiling($needHere)) GB needed."
        }
    }
    if ($sys -and $needSys -gt 0 -and $sys.FreeGB -lt $needSys) {
        Add "stop" ("Not enough free space on Windows' drive $($sys.Root) for the programs the build needs: $($sys.FreeGB) GB free, about " +
            "$([math]::Ceiling($needSys)) GB needed (Microsoft's C++ Build Tools go there even when the game is on another drive). Free some space on $($sys.Root), then run the setup again.")
    }

    # --- the disc -------------------------------------------------------------------------------------------------------
    # (no Python yet on a fresh PC: the setup installs it first, and the build checks the disc again then)
    if ($Iso -and $Python -and (Test-Path -LiteralPath $Iso -PathType Leaf)) {
        $tool = Join-Path $Port "transformers\tools\extract_iso.py"
        if (Test-Path -LiteralPath $tool) {
            $ErrorActionPreference = "Continue"
            $r = "$(& $Python $tool --check $Iso 2>&1 | Out-String)".Trim()
            $code = $LASTEXITCODE
            $ErrorActionPreference = "Stop"
            if ($code -eq 0) { Add "info" "The disc: $r" }
            elseif ($code -eq 2) { Add "stop" $r }
            else { Add "warn" "The disc could not be checked ahead: $r (the build checks it again)" }
        }
    }

    # --- Windows --------------------------------------------------------------------------------------------------------
    try {
        $sac = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy" -Name VerifiedAndReputablePolicyState -ErrorAction Stop).VerifiedAndReputablePolicyState
        if ($sac -eq 1) {
            Add "warn" ("Windows' Smart App Control is on. The game is built on this PC and is not signed, so Smart App Control may refuse " +
                "it or one of its files when it starts. If it does: Windows Security > App & browser control > Smart App Control settings > Off " +
                "(on some Windows 11 versions it cannot be turned back on without resetting Windows).")
        }
    } catch {}
    try {
        $av = @(Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction Stop | ForEach-Object displayName | Select-Object -Unique)
        if ($av.Count) { Add "info" "Antivirus: $($av -join ', ')" }
        $other = @($av | Where-Object { $_ -notmatch 'Windows Defender|Microsoft Defender' })
        if ($other.Count) {
            Add "info" ("If the build stops at a program it just made, or a step hangs for many minutes, $($other -join ' / ') may be holding it: " +
                "allow the game's folder ($full) in it, then run the setup again.")
        }
    } catch {}
    try {
        $ram = [math]::Round((Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory / 1GB)
        if ($ram -lt 8) { Add "warn" "This PC has $ram GB of memory: the build needs about 8 GB and may be very slow. 'Go easy on my PC' uses less at a time." }
    } catch {}
    return $out
}

function Write-PreflightReport($Findings) {
    $stop = $false
    foreach ($f in $Findings) {
        switch ($f.Level) {
            "stop" { Write-Host "  STOP: $($f.Text)" -ForegroundColor Red; $stop = $true }
            "warn" { Write-Host "  note: $($f.Text)" -ForegroundColor Yellow }
            default { Write-Host "  $($f.Text)" }
        }
        if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "preflight: $($f.Level): $($f.Text)" }
    }
    return -not $stop
}

# Run on its own: the findings printed, exit 1 when one stops.
if ($MyInvocation.InvocationName -ne '.') {
    $o = @{ Iso = ""; Port = (Split-Path $PSScriptRoot -Parent); Update = $false; HdQuality = "" }
    for ($i = 0; $i -lt $args.Count; $i++) {
        switch -regex ("$($args[$i])") {
            '^-Iso$' { $o.Iso = $args[++$i] }
            '^-Port$' { $o.Port = $args[++$i] }
            '^-HdQuality$' { $o.HdQuality = $args[++$i] }
            '^-Update$' { $o.Update = $true }
        }
    }
    . (Join-Path $PSScriptRoot "install_layout.ps1")
    $ok = Write-PreflightReport (Invoke-Preflight -Iso $o.Iso -Port $o.Port -Update:$o.Update -HdQuality $o.HdQuality)
    if (-not $ok) { exit 1 }
}
