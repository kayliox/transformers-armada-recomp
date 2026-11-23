# setup_common.ps1 - what setup.ps1 and update.ps1 share (dot-sourced: . tools\setup_common.ps1).
# Also used by a repository that holds this folder as a subfolder (its own setup.ps1 / update.ps1
# call ours with -Root and -Hooks), so there is one code path.

# Runs a native program's probe without its error output stopping the script (Windows PowerShell
# turns a native program's stderr into an error under ErrorActionPreference "Stop").
function Invoke-Probe([scriptblock]$Block) {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Block 2>$null } finally { $ErrorActionPreference = $eap }
}

# Runs a native program with its output shown (stderr too, as plain lines) and never stopped by
# what it writes to stderr: under "Stop", Windows PowerShell turns a native program's stderr into a
# terminating error as soon as anything redirects the script's output (pip's notices, git's
# progress), which ended a redirected setup right after "Installing CMake". Returns its exit code.
function Invoke-Native([scriptblock]$Block) {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Block 2>&1 | ForEach-Object { Write-Host "$_" } } finally { $ErrorActionPreference = $eap }
    return $LASTEXITCODE
}

# PATH as the registry has it now (programs installed since this PowerShell started), our own first.
function Update-PathFromRegistry {
    $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $user = [Environment]::GetEnvironmentVariable("Path", "User")
    $env:PATH = "$env:PATH;$machine;$user"
}

# A real Python 3.11 or newer - never Windows' "python" shortcut (WindowsApps\python.exe, an App
# execution alias that only opens the Microsoft Store: found on a PC without Python, 2026-10-30).
# Returns its python.exe, or $null. Use-Python puts its folders first on PATH, so build.ps1 and the
# tools it starts find the same one.
function Find-Python {
    $candidates = @(Get-PythonCandidates)
    foreach ($exe in $candidates) {
        # (3.11 or newer WITH pip: a Python bundled inside another program - Inkscape's, found on a player's PATH -
        # has no pip, and the build installs its tools with pip)
        if ((Test-PythonExe $exe) -eq "ok") { return $exe }
    }
    return $null
}

# Every python.exe on this PC the setup could use, best first: PATH (not Windows' Store shortcut), the py launcher's
# list, the usual install folders (for this user and for all users), and the registry's record of each install
# (PEP 514: HKCU/HKLM\Software\Python\PythonCore\<version>\InstallPath) - a Python installed without "Add python.exe to
# PATH", or in a folder of the player's own, which winget then calls "already installed" (a player's setup, 2026-11-11).
function Get-PythonCandidates {
    $c = @()
    $c += Get-Command python -All -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -and $_.Source -notlike "*\WindowsApps\*" } | ForEach-Object Source
    if (Get-Command py -ErrorAction SilentlyContinue) {
        $c += Invoke-Probe { & py -3 -c "import sys; print(sys.executable)" }
        # (py -0p: every Python the launcher knows, "-V:3.12 *  C:\...\python.exe")
        $c += Invoke-Probe { & py -0p } | ForEach-Object { if ("$_" -match '([A-Za-z]:\\.*python\.exe)\s*$') { $Matches[1] } }
    }
    $c += Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe", "$env:ProgramFiles\Python3*\python.exe",
        "${env:ProgramFiles(x86)}\Python3*\python.exe", "$env:SystemDrive\Python3*\python.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | ForEach-Object FullName
    foreach ($hive in "HKCU:", "HKLM:") {
        foreach ($key in "$hive\Software\Python\PythonCore", "$hive\Software\WOW6432Node\Python\PythonCore") {
            foreach ($v in @(Get-ChildItem $key -ErrorAction SilentlyContinue | Sort-Object PSChildName -Descending)) {
                $ip = Get-ItemProperty (Join-Path $v.PSPath "InstallPath") -ErrorAction SilentlyContinue
                if ($ip -and $ip.ExecutablePath) { $c += $ip.ExecutablePath }
                elseif ($ip -and $ip.'(default)') { $c += (Join-Path $ip.'(default)' "python.exe") }
            }
        }
    }
    # (TODO 444) Never a Python inside a WindowsApps folder, from any source above: Windows' Store shortcut (the PATH
    # alias that opens the Store) and the Microsoft Store's Python itself. The py launcher and the registry name the
    # Store Python's real exe (C:\Program Files\WindowsApps\PythonSoftwareFoundation.Python.3.13_...\python3.13.exe):
    # it answered the setup's probe, then the build's own "python" in that folder stopped with "Access denied" (a
    # player's setup, 2026-11-19). Without another Python the setup installs Python 3.12, as on a PC with none.
    $script:SkippedStorePythons = @($c | Where-Object { $_ -and (Test-StorePythonPath $_) } | Select-Object -Unique)
    return @($c | Where-Object { $_ -and -not (Test-StorePythonPath $_) -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)
}

# (TODO 444) Whether a python.exe path is inside a WindowsApps folder (the Microsoft Store's Python or Windows' Store
# shortcut): never used by the setup or the build.
function Test-StorePythonPath([string]$Path) {
    return ("$Path" -like '*\WindowsApps\*')
}

# One python.exe: "ok" (3.11 or newer with pip), "old" (older than 3.11), "nopip" (3.11+ without pip) or "broken".
function Test-PythonExe([string]$Exe) {
    $r = Invoke-Probe { & $Exe -c "import sys, importlib.util; p = importlib.util.find_spec('pip') is not None; print('%d.%d %d' % (sys.version_info[0], sys.version_info[1], p))" }
    $script:LastPythonProbe = ("$r" -replace '\s+', ' ').Trim()
    $m = [regex]::Match("$r", '(\d+)\.(\d+) (\d)')
    if (-not $m.Success) { return "broken" }
    if ([int]$m.Groups[1].Value -lt 3 -or ([int]$m.Groups[1].Value -eq 3 -and [int]$m.Groups[2].Value -lt 11)) { return "old" }
    if ($m.Groups[3].Value -ne "1") { return "nopip" }
    return "ok"
}

# A Python 3.11+ that is there but has no pip (installed without it): pip added with its own ensurepip, for this user
# only (no administrator rights needed). Returns the python.exe that now works, or $null.
function Repair-PythonPip {
    foreach ($exe in @(Get-PythonCandidates)) {
        if ((Test-PythonExe $exe) -ne "nopip") { continue }
        Write-Host "Python $exe has no pip: adding it (ensurepip)"
        Invoke-Native { & $exe -m ensurepip --user --upgrade } | Out-Null
        if ((Test-PythonExe $exe) -eq "ok") { return $exe }
    }
    return $null
}

# What the setup found of Python, in a line for a player (why none of them could be used).
function Get-PythonFindings {
    $found = @(Get-PythonCandidates | ForEach-Object {
        $t = Test-PythonExe $_
        $why = switch ($t) { "old" { "older than 3.11" } "nopip" { "has no pip" } "broken" { "does not start" } default { "usable" } }
        $ver = [regex]::Match($script:LastPythonProbe, '^(\d+\.\d+)').Groups[1].Value
        if (-not $ver) { $ver = "?" }
        "$_ (Python $ver, $why)"
    })
    $store = if (@($script:SkippedStorePythons).Count) { "; the Microsoft Store's Python, never used (Windows does not let other programs run it directly): " + (@($script:SkippedStorePythons) -join "; ") } else { "" }
    if ($found.Count -eq 0) { return "no python.exe found (not on PATH, not in the py launcher, the usual folders or the registry)$store" }
    return "found: " + ($found -join "; ") + $store
}

function Use-Python([string]$Exe) {
    $dir = Split-Path $Exe
    $userScripts = Invoke-Probe { & $Exe -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))" }
    $env:PATH = "$dir;$dir\Scripts;$userScripts;$env:PATH"
    # (TODO 444) The exact python.exe chosen, for build.ps1 and the programs it starts (they run this one, not whatever
    # "python" PATH finds first)
    $env:TF_PYTHON = $Exe
}

# Fonts improve prompts, but must not prevent an otherwise playable install or its mods.
function Invoke-OptionalFonts([string]$Python, [string]$Port) {
    try {
        # (TODO 382: numpy checked and installed by make_hd_textures.ps1 -FontsOnly itself, each with a time limit - never
        # here, where a stuck import or pip would hold the setup or the update for good)
        & (Join-Path $Port "tools\make_hd_textures.ps1") -FontsOnly -Python $Python
        if (-not $?) { throw "making the fonts failed" }
    } catch {
        Write-Host "`nwarning: $($_.Exception.Message): the button prompts and sharp fonts are not made; continuing with the game's own fonts." -ForegroundColor Yellow
    }
}

# Transformers (USA), SLUS-20668, the disc the build is made for, and the European disc, SLES-52388 (En, Fr, De, Es,
# It; experimental). The build checks the disc's executable; the sizes only tell the setup which one it is given.
function Get-UsaDiscSize { 2900951040 }
function Get-PalDiscSize { 4546691072 }

# (hotfix) The oldest Visual Studio the game builds with (build.ps1's $MinVsVersion): 2019 version 16.11. An older install
# with the C++ tools (Visual Studio 2017) does not count: the setup installs Visual Studio 2022's Build Tools beside it.
function Get-VsMinVersion { [version]"16.11" }

# What the build needs and is not there: "git", "python", "vs" (Visual Studio's C++ tools, 2019 16.11 or newer).
function Get-MissingPrerequisites {
    $m = @()
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $m += "git" }
    if (-not (Find-Python)) { $m += "python" }   # (a real one: not Windows' Microsoft Store shortcut)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $min = "[$(Get-VsMinVersion),"
    $vs = (Test-Path $vswhere) -and [bool](Invoke-Probe { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -version $min -property installationPath })
    # (the C++ tools alone are not enough: without the Windows SDK, CMake's compiler test fails with "No
    # CMAKE_C_COMPILER could be found" - a player's setup, 2026-11-10. The SDK counts as part of "vs".)
    if (-not $vs -or -not (Test-WindowsSdk)) { $m += "vs" }
    return $m
}

# Whether a Windows 10/11 SDK the C++ build can use is installed: its headers and its x64 libraries.
function Test-WindowsSdk {
    $roots = @(${env:ProgramFiles(x86)}, $env:ProgramFiles) | Where-Object { $_ } | ForEach-Object { Join-Path $_ "Windows Kits\10" }
    foreach ($r in $roots) {
        $inc = Join-Path $r "Include"
        if (-not (Test-Path -LiteralPath $inc)) { continue }
        foreach ($v in @(Get-ChildItem -LiteralPath $inc -Directory -Filter "10.*" -ErrorAction SilentlyContinue)) {
            $lib = Join-Path $r "Lib\$($v.Name)"
            if ((Test-Path -LiteralPath (Join-Path $v.FullName "um\windows.h")) -and
                (Test-Path -LiteralPath (Join-Path $lib "um\x64\kernel32.lib")) -and
                (Test-Path -LiteralPath (Join-Path $lib "ucrt\x64\ucrt.lib"))) { return $true }
        }
    }
    return $false
}

# (TODO 407) The Visual Studio installs with the C++ tools that this CMake does not know, when it knows none of them
# (Visual Studio 2026 alone, and a CMake from before it): their names - the build would stop at "could not find any
# instance of Visual Studio". Empty when one is usable, or nothing to say. $List/$CMakeHelp: for the tests.
function Get-VsUnknownToCmake($List = $null, $CMakeHelp = $null) {
    if ($null -eq $List) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) { return @() }
        $json = "$(Invoke-Probe { & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json } | Out-String)"
        try { $List = @($json | ConvertFrom-Json | ForEach-Object { $_ }) } catch { return @() }
    }
    if ($null -eq $CMakeHelp) { $CMakeHelp = "$(Invoke-Probe { & cmake --help } | Out-String)" }
    $names = @{ 15 = "Visual Studio 15 2017"; 16 = "Visual Studio 16 2019"; 17 = "Visual Studio 17 2022"; 18 = "Visual Studio 18 2026" }
    $known = @(); $unknown = @()
    foreach ($i in @($List | Where-Object { $_.installationPath -and $_.installationVersion })) {
        $major = [int]("$($i.installationVersion)" -split '\.')[0]
        if ($names.ContainsKey($major) -and $CMakeHelp.Contains($names[$major])) { $known += $i } else { $unknown += "$(if ($i.displayName) { $i.displayName } else { "Visual Studio $major" }) $($i.installationVersion)" }
    }
    if ($known.Count) { return @() }
    return @($unknown)
}

# Visual Studio installs (any edition, the Build Tools too) the setup can repair: their folders, the one to use first.
# winget sees such an install as "already installed" and adds nothing, so the setup kept stopping with "not found yet:
# vs" (issue #2, 2026-11-09; again 2026-11-11, a player's "No available upgrade found"). Three kinds:
#   - an install WITHOUT the C++ tools (Install-VsCppTools adds them with Visual Studio's own installer);
#   - an install with them but no Windows SDK (the same: the workload's recommended parts bring the SDK);
#   - an INCOMPLETE install (interrupted, or waiting for a restart): vswhere lists it only with -all, so the check
#     (-latest, complete installs only) never saw it while the old list here counted it as one with the C++ tools.
function Get-VsWithoutCpp {
    return @(Get-VsToRepair | ForEach-Object { $_.Path })
}

# The installs as objects: @{ Path; Complete; Cpp; Version; TooOld } (vswhere -all; its JSON says whether each is complete).
function Get-VsInstances {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return @() }
    $json = "$(Invoke-Probe { & $vswhere -all -products * -format json } | Out-String)"
    $cpp = @(Invoke-Probe { & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } | Where-Object { $_ })
    $list = @()
    try { $list = @($json | ConvertFrom-Json | ForEach-Object { $_ }) } catch { }
    return @($list | Where-Object { $_.installationPath } | ForEach-Object {
        $v = $null
        try { $v = [version]"$($_.installationVersion)" } catch { }
        [pscustomobject]@{ Path = $_.installationPath; Complete = ($_.isComplete -ne $false); Cpp = ($cpp -contains $_.installationPath)
                           Version = $v; TooOld = ($null -ne $v -and $v -lt (Get-VsMinVersion)) } })
}

# The installs to repair, best first: @{ Path; Action = "resume" (finish an incomplete one) or "modify" (add the C++ tools) }.
# (hotfix) Never one too old for the game (Visual Studio 2017): repaired it would still not build it, so the setup installs
# Visual Studio 2022's Build Tools instead.
function Get-VsToRepair {
    $all = @(Get-VsInstances | Where-Object { -not $_.TooOld })
    $sdk = Test-WindowsSdk
    $out = @()
    $out += $all | Where-Object { -not $_.Complete } | ForEach-Object { [pscustomobject]@{ Path = $_.Path; Action = "resume" } }
    $out += $all | Where-Object { $_.Complete -and $_.Cpp -and -not $sdk } | ForEach-Object { [pscustomobject]@{ Path = $_.Path; Action = "modify" } }
    $out += $all | Where-Object { $_.Complete -and -not $_.Cpp } | ForEach-Object { [pscustomobject]@{ Path = $_.Path; Action = "modify" } }
    return @($out)
}

# What the setup found of Visual Studio, in a line for a player.
function Get-VsFindings {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        $dirs = @(Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\20*\*", "$env:ProgramFiles\Microsoft Visual Studio\*\*" -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
        if ($dirs.Count) { return "Visual Studio folders are there ($($dirs -join '; ')) but its installer (vswhere.exe) is missing: the install is broken" }
        return "no Visual Studio install found (its installer, vswhere.exe, is not on this PC)"
    }
    $i = @(Get-VsInstances)
    if ($i.Count -eq 0) { return "Visual Studio's installer is there but lists no install" }
    $sdk = if (Test-WindowsSdk) { "a Windows SDK is there" } else { "no Windows 10/11 SDK" }
    return "found: " + (($i | ForEach-Object { "$($_.Path)$(if ($_.Version) { " $($_.Version)" }) (" + $(if ($_.TooOld) { "too old for the game: it needs 2019 version $(Get-VsMinVersion) or newer" } elseif (-not $_.Complete) { "incomplete" } elseif ($_.Cpp) { "with the C++ tools" } else { "WITHOUT the C++ tools" }) + ")" }) -join "; ") + "; $sdk"
}

# The evidence behind the checks, written into the setup's log (setup.log): every Python found and what it answered,
# every Visual Studio install vswhere lists (complete or not, with the C++ tools or not), whether a Windows SDK is
# there, and what Windows' list of installed programs says about Python and Visual Studio (for this user or all users,
# where) - winget decides "already installed" from that list. A player's log once held only "not found yet: python,
# vs", which explained nothing (2026-11-11).
function Write-PrerequisiteEvidence([string]$When) {
    Write-Host "`n--- what the setup sees ($When) ---"
    Write-Host "Windows: $([Environment]::OSVersion.VersionString), $env:PROCESSOR_ARCHITECTURE; winget: $(if ($p = Get-WingetProblem) { $p } else { 'usable, ' + "$(Invoke-Probe { & winget --version })".Trim() })"
    Write-Host "Python: $(Get-PythonFindings)"
    $alias = Get-Command python -All -ErrorAction SilentlyContinue | Where-Object { $_.Source -like "*\WindowsApps\*" } | Select-Object -First 1
    if ($alias) { Write-Host "  (also Windows' Microsoft Store shortcut $($alias.Source): not a Python, never used)" }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    Write-Host "vswhere: $(if (Test-Path $vswhere) { $vswhere } else { 'missing' })"
    if (Test-Path $vswhere) {
        $json = "$(Invoke-Probe { & $vswhere -all -prerelease -products * -format json } | Out-String)"
        $cpp = @(Invoke-Probe { & $vswhere -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } | Where-Object { $_ })
        $list = @(); try { $list = @($json | ConvertFrom-Json | ForEach-Object { $_ }) } catch { }
        if ($list.Count -eq 0) { Write-Host "  no install listed" }
        foreach ($i in $list) {
            Write-Host ("  {0} {1} at {2}: complete {3}, launchable {4}, C++ tools {5}" -f $i.displayName, $i.installationVersion, $i.installationPath, $i.isComplete, $i.isLaunchable, ($cpp -contains $i.installationPath))
        }
    }
    Write-Host "Windows SDK (headers + x64 libraries): $(if (Test-WindowsSdk) { 'there' } else { 'missing' })"
    $av = @(Get-AntivirusProducts)
    Write-Host "antivirus programs: $(if ($av.Count) { $av -join ', ' } else { 'none reported by Windows Security' })"
    $keys = @("HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*", "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
              "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*")
    foreach ($k in $keys) {
        foreach ($e in @(Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -match '^Python 3\.\d+\.\d+ \(|^Python 3\.\d+\.\d+$|Visual Studio (Build Tools|Community|Professional|Enterprise) 20' })) {
            $scope = if ($k -like "HKCU:*") { "this user" } else { "all users" }
            Write-Host "Installed programs ($scope): $($e.DisplayName) $($e.DisplayVersion)$(if ($e.InstallLocation) { ' at ' + $e.InstallLocation })"
        }
    }
    Write-Host "--- end ---"
}

# The antiviruses Windows knows of (Windows Security's list), for the log.
function Get-AntivirusProducts {
    try { return @(Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction Stop | ForEach-Object displayName) }
    catch { return @() }
}

# A program built on this PC blocked by an antivirus (build.ps1's message): what the log should hold - where that file
# is (or that it is gone), the antiviruses on the PC, and Windows Defender's last detections when it is the one.
function Write-AntivirusEvidence([string]$File, [string]$Port) {
    Write-Host "`n--- antivirus ($File) ---"
    $found = @(Get-ChildItem -LiteralPath (Join-Path $Port "out") -Recurse -Filter $File -ErrorAction SilentlyContinue | ForEach-Object FullName)
    Write-Host "the file: $(if ($found.Count) { $found -join '; ' } else { 'not in out\ any more (quarantined or deleted)' })"
    $av = @(Get-AntivirusProducts)
    Write-Host "antivirus programs: $(if ($av.Count) { $av -join ', ' } else { 'none reported by Windows Security' })"
    try {
        # (only detections in the game's folder: the log may be shared, and other files on the PC are not ours to list)
        $d = @(Get-MpThreatDetection -ErrorAction Stop | Where-Object { ($_.Resources -join ' ') -like "*$Port*" } |
               Sort-Object InitialDetectionTime -Descending | Select-Object -First 5)
        if ($d.Count -eq 0) { Write-Host "Windows Defender: no detection in the game's folder" }
        foreach ($x in $d) { Write-Host "Windows Defender detection $($x.InitialDetectionTime): $(($x.Resources | Select-Object -First 2) -join ', ')" }
    } catch { }
    Write-Host "--- end ---"
}

# Adds the C++ tools ("Desktop development with C++" / the VCTools workload) to an existing Visual Studio install, or
# finishes an incomplete one (-Action resume). Windows asks for permission once (the installer needs admin rights).
# Returns the installer's exit code (1223: Windows' question declined or unanswered).
function Install-VsCppTools([string]$Path, [string]$Action = "modify") {
    $installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\setup.exe"
    if (-not (Test-Path $installer)) { return -1 }
    $workload = if ((Split-Path $Path -Leaf) -eq "BuildTools") { "Microsoft.VisualStudio.Workload.VCTools" } else { "Microsoft.VisualStudio.Workload.NativeDesktop" }
    $a = if ($Action -eq "resume") { @("resume", "--installPath", "`"$Path`"", "--passive", "--norestart") }
         else { @("modify", "--installPath", "`"$Path`"", "--add", $workload, "--includeRecommended", "--passive", "--norestart") }
    try {
        $p = Start-Process -FilePath $installer -ArgumentList $a -Verb RunAs -Wait -PassThru
        return $p.ExitCode
    } catch {
        Write-Host "warning: Visual Studio's installer did not start: $($_.Exception.Message)" -ForegroundColor Yellow
        # (Windows' permission question declined or closed unanswered: "The operation was canceled by the user")
        if ($_.Exception.NativeErrorCode -eq 1223 -or $_.Exception.Message -match "canceled by the user|cancelled by the user") { return 1223 }
        return -1
    }
}

# Each prerequisite's name and its winget command (-InstallPrerequisites runs them).
# (--source winget: the Microsoft Store source is not needed and its own agreement and outages stopped installs;
# --disable-interactivity: no question waits on a console nobody sees. VS: --passive shows its installer's progress
# - several GB, quiet for long minutes otherwise - and --norestart never restarts the PC under the player.)
function Get-PrerequisiteInstall {
    $accept = "--source winget --accept-package-agreements --accept-source-agreements --disable-interactivity"
    @{
        git    = @("Git", "winget install --id Git.Git -e $accept")
        python = @("Python 3.11 or newer", "winget install --id Python.Python.3.12 -e --scope user $accept")
        vs     = @("Visual Studio 2022 Build Tools with the C++ tools",
                   "winget install --id Microsoft.VisualStudio.2022.BuildTools -e $accept --override `"--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"")
    }
}

# What a player does by hand for each prerequisite, when the setup could not install it (links included).
function Get-PrerequisiteManualSteps {
    @{
        git    = "Git: download it from https://git-scm.com/download/win , run it and keep the default choices."
        python = "Python: download Python 3.12 from https://www.python.org/downloads/windows/ (the 'Windows installer (64-bit)'), run it and TICK 'Add python.exe to PATH' on its first page."
        vs     = "Visual Studio Build Tools: download them from https://visualstudio.microsoft.com/visual-cpp-build-tools/ , run them, tick 'Desktop development with C++' (keep the Windows 11 SDK ticked on the right) and press Install (about 2 GB to download, 6 GB on disk). Already have Visual Studio or its Build Tools? Open 'Visual Studio Installer' from the Start menu, press Modify and tick the same 'Desktop development with C++'."
        winget = "winget (Windows' package manager) is missing or too old: install or update 'App Installer' from the Microsoft Store, https://apps.microsoft.com/detail/9NBLGGH4NNS1 , then run the setup again. (Windows Server and LTSC editions have no Store: install the tools by hand with the links below.)"
    }
}

# (TODO 439) The sizes and times the setup's plan says, measured on a fresh install (the Testbench: Ryzen 5 5600, NVMe,
# GTX 1080, 2026-11-19; README "Install size"). Download / disk in MB / GB. Keep in step with installer\src\main.rs
# (PLAN_TOOLS and the PLAN_* numbers).
function Get-SetupSizes {
    @{
        git      = @{ Download = 67; Disk = 0.4; Why = "gets the game's build recipe (this repository) and its updates" }
        python   = @{ Download = 27; Disk = 0.2; Why = "runs the build's tools and makes the HD textures" }
        vs       = @{ Download = 1900; Disk = 6.1; Why = "Microsoft's free C++ compiler: the game is built on this PC from your own disc, since no game code may be shared" }
        pyparts  = 0.3     # Pillow, numpy and CMake for that Python (pip)
        disc     = 2.7     # the disc's files, extracted from your disc image
        build    = 2.3     # the build: the game itself (0.3) and its in-between files (tools\clean_build.ps1 can delete those); measured 2026-11-19 with the libraries as archives, was 3.4 with their git histories
        gameMin  = 12; gameMax = 25   # the build, minutes (the Testbench, 12 threads: 10 min 37 s)
    }
}

# (TODO 439) An HD textures preset's set on the disk (GB) and its making time on an RTX 3070-class card (minutes):
# tools\hd_textures\tier_rules.py ESTIMATES, as installer\src\main.rs HdQuality says them.
function Get-HdPresetCost([string]$Quality) {
    switch ($Quality) { "fast" { return @(0.6, 10) } "best" { return @(2.3, 33) } default { return @(1.4, 25) } }
}

# (TODO 439) The setup's plan, said before anything runs: each program with why it is needed, its download and disk
# size, and whether it is already installed ($Missing: Get-MissingPrerequisites), then the game, the HD textures, and the
# space and time in all. $HdGB / $HdMinutes: the HD textures preset's (0 when they are left out).
function Get-SetupPlanText([string[]]$Missing, [double]$HdGB = 0, [int]$HdMinutes = 0) {
    $s = Get-SetupSizes
    $names = @{ git = "Git"; python = "Python 3.12"; vs = "Visual Studio 2022 Build Tools (C++)" }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("The programs the build needs (installed with Windows' package manager, winget, only when missing):")
    $disk = 0.0; $download = 0
    foreach ($k in "git", "python", "vs") {
        $t = $s[$k]
        if ($Missing -contains $k) {
            $dl = if ($t.Download -ge 1000) { "{0:N1} GB" -f ($t.Download / 1000) } else { "$($t.Download) MB" }
            $lines.Add(("  {0}: to install ({1} download, {2:N1} GB on disk) - {3}" -f $names[$k], $dl, $t.Disk, $t.Why))
            $disk += $t.Disk; $download += $t.Download
        } else {
            $lines.Add(("  {0}: already installed - {1}" -f $names[$k], $t.Why))
        }
    }
    $lines.Add(("  Pillow, numpy and CMake for Python (pip, when missing): about {0:N1} GB" -f $s.pyparts))
    $disk += $s.pyparts + $s.disc + $s.build
    $lines.Add(("The game, built on this PC from your disc: {0:N1} GB of disc files and {1:N1} GB of build, about {2} to {3} min." -f $s.disc, $s.build, $s.gameMin, $s.gameMax))
    if ($HdGB -gt 0) {
        $disk += $HdGB
        $lines.Add(("HD textures (optional): about {0:N1} GB, about {1} min on the graphics card; you can play meanwhile." -f $HdGB, $HdMinutes))
    } else {
        $lines.Add("HD textures: left out (the game shows its original textures).")
    }
    $lines.Add(("In all: about {0:N0} GB of free space{1}. Nothing else is installed." -f [Math]::Ceiling($disk), $(if ($download) { (", about {0:N1} GB to download" -f ($download / 1000)) } else { "" })))
    $lines.Add("After the build, tools\clean_build.ps1 can free most of the build's space (the setup window offers it).")
    return @($lines)
}

# winget, if it can be used: $null when it can, else why not. (Windows 10 LTSC / Server and old Windows 10 have no
# App Installer; an old App Installer's winget cannot read today's packages; a broken one does not even start.)
function Get-WingetProblem {
    $cmd = Get-Command winget -ErrorAction SilentlyContinue
    if (-not $cmd) { return "winget is not installed on this PC" }
    $v = "$(Invoke-Probe { & winget --version })".Trim()
    if ($LASTEXITCODE -ne 0 -or -not $v) { return "winget is installed but does not start (exit code $LASTEXITCODE)" }
    $m = [regex]::Match($v, '(\d+)\.(\d+)')
    if ($m.Success -and ([int]$m.Groups[1].Value -lt 1 -or ([int]$m.Groups[1].Value -eq 1 -and [int]$m.Groups[2].Value -lt 6))) {
        return "winget $v is too old (1.6 or newer is needed)"
    }
    return $null
}

# (TODO 438) Whether winget's words say its source (the package list it installs from) is damaged or missing: every
# winget install then fails the same way, Git, Python and the Build Tools alike. A player's screen (2026-11-18): "Failed
# when opening source(s); try the 'source reset' command if the problem persists. ... 0x8a15000f : Data required by the
# source is missing". (0x8a15000f = -1978335217 data missing, 0x8a150044 = -1978335164; TODO 443: also as winget may print
# them, unsigned: 2316632079, 2316632132; the words cover the rest: a
# removed source, a list that could not be updated.) Keep in step with installer\src\main.rs winget_source_broken.
function Test-WingetSourceBroken([string]$Output) {
    return ($Output -match "0x8a15000f|0x8a150044|-1978335217|-1978335164|2316632079|2316632132|Data required by the source is missing|Failed when opening source|Failed when searching source|Failed in attempting to update the source|source reset|No sources match the given value|no sources are configured")
}

# (TODO 438) What a player does when winget's source stays damaged (the setup's own repair did not help or was declined).
function Get-WingetSourceText {
    return "Windows' package manager (winget) has a damaged package list, so the setup cannot install the tools with it. To repair it: right-click the Start button, open 'Terminal (Admin)' (or 'Windows PowerShell (Admin)'), run: winget source reset --force , then run the setup again. Or install the tools by hand (links below)."
}

# (TODO 438) Repairs winget's source: "winget source reset --force", which needs administrator rights, so Windows asks for
# permission (the same way as Visual Studio's installer above). Its output goes to a file (an elevated program's output
# cannot be read directly) and is shown here. Returns @{ Code; Output } (1223: Windows' question declined or unanswered).
function Repair-WingetSource {
    $file = Join-Path $env:TEMP "tfa_winget_source_reset.txt"
    Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    Write-Host "`n==> Repairing winget's package list (winget source reset --force). Windows asks for permission: click Yes" -ForegroundColor Cyan
    try {
        $p = Start-Process -FilePath "cmd.exe" -ArgumentList "/c winget source reset --force > `"$file`" 2>&1" -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        $code = $p.ExitCode
    } catch {
        if ($_.Exception.NativeErrorCode -eq 1223 -or $_.Exception.Message -match "canceled by the user|cancelled by the user") {
            Write-Host "winget source reset: Windows' permission was not given"
            return @{ Code = 1223; Output = "winget source reset: Windows' permission was not given" }
        }
        Write-Host "winget source reset did not start: $($_.Exception.Message)"
        return @{ Code = -1; Output = "winget source reset did not start: $($_.Exception.Message)" }
    }
    $text = if (Test-Path -LiteralPath $file) { (Get-Content -LiteralPath $file -ErrorAction SilentlyContinue) -join "`n" } else { "" }
    Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    if ($text) { Write-Host $text }
    Write-Host "winget source reset: exit code $code"
    return @{ Code = $code; Output = "$text`nwinget source reset: exit code $code" }
}

# (TODO 443) An exit code as the signed 32-bit number Windows and PowerShell use ($LASTEXITCODE, Process.ExitCode), from
# a number or from text that may give it unsigned: winget prints an installer's code as "2147942512" (= 0x80070070,
# ERROR_DISK_FULL), past [int]'s range, and a plain [int] cast of it stopped a player's setup with "Cannot convert value
# 2147942512 to type System.Int32" instead of saying the disk was full. Decimal (signed or unsigned) or 0x hex; $null
# when it is not a 32-bit exit code.
function ConvertTo-ExitCode($Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [int]) { return $Value }
    $s = "$Value".Trim()
    [long]$n = 0
    if ($s -match '^0x([0-9a-fA-F]{1,8})$') { $n = [Convert]::ToInt64($Matches[1], 16) }
    elseif (-not ($s -match '^[+-]?\d{1,19}$' -and [long]::TryParse($s, [ref]$n))) { return $null }
    if ($n -gt 4294967295 -or $n -lt -2147483648) { return $null }
    if ($n -gt 2147483647) { $n -= 4294967296 }
    return [int]$n
}

# (TODO 443) An exit code as a player and a search engine see it: "2147942512 (0x80070070)" - the unsigned number winget
# prints and the hex Microsoft's pages use. Text that is not a code is given back as it is.
function Format-ExitCode($Code) {
    $c = ConvertTo-ExitCode $Code
    if ($null -eq $c) { return "$Code" }
    if ($c -ge 0 -and $c -lt 65536) { return "$c" }
    $u = if ($c -lt 0) { [long]$c + 4294967296 } else { [long]$c }
    return ("{0} (0x{1:X8})" -f $u, $u)
}

# (TODO 443) The free space on Windows' own drive (GB, one decimal), $null when it cannot be read. The Build Tools go
# there whatever drive the game is on.
function Get-SystemDriveFreeGB {
    try { return [math]::Round(([IO.DriveInfo]::new("$env:SystemDrive\")).AvailableFreeSpace / 1GB, 1) } catch { return $null }
}

# What an installer's exit code and last lines mean, in words a player can act on ("" when nothing is known).
# $Code: a number or text (TODO 443: never cast with [int] - an HRESULT printed unsigned overflows it).
# (TODO 443) The installer's own exit code: winget's says only "the installer failed", the installer's is in its output
# ("Installer failed with exit code: N") - the last one when several installers ran (winget's, then the maker's own).
# Else $Code. Signed Int32 (ConvertTo-ExitCode), or $null.
function Get-InstallerExitCode($Code, [string]$Output) {
    $ms = [regex]::Matches("$Output", "Installer failed with exit code: (-?\d+|0x[0-9a-fA-F]+)")
    if ($ms.Count) { $c = ConvertTo-ExitCode $ms[$ms.Count - 1].Groups[1].Value; if ($null -ne $c) { return $c } }
    return (ConvertTo-ExitCode $Code)
}

function Get-InstallFailureMeaning($Code, [string]$Output) {
    $Code = Get-InstallerExitCode $Code $Output
    # (TODO 443) Disk full: ERROR_DISK_FULL 112 / ERROR_HANDLE_DISK_FULL 39, as themselves or as HRESULTs (0x80070070 =
    # 2147942512 = -2147024784, 0x80070027). First: a full disk makes other installers' words misleading.
    if ($Code -in 112, 39, -2147024784, -2147024857 -or
        $Output -match "0x80070070|0x80070027|2147942512|2147942439|-2147024784|-2147024857|not enough (free )?(disk )?space|There is not enough space|disk is full") {
        $free = Get-SystemDriveFreeGB
        $now = if ($null -ne $free) { " (it has $free GB free now)" } else { "" }
        return ("not enough free disk space. Microsoft's C++ Build Tools need about 7 GB free on Windows' drive $env:SystemDrive$now, " +
                "even when the game goes on another drive; Git and Python about 1 GB more. Free some space on $env:SystemDrive " +
                "(Windows Settings > System > Storage, or Disk Cleanup), then run the setup again: finished steps are kept.")
    }
    if ($Output -match "You cancelled the installation" -or $Code -in 2, 1223, 1602, 5004, -2147023673) {
        # (2: Git's installer (Inno Setup) when Windows' question was declined or closed itself unanswered - the ThinkPad test)
        return "Windows asked for permission to install and nobody answered (or No was chosen). The question may show only as a flashing shield button in the taskbar, and it closes by itself after about 2 minutes. Run the setup again and click Yes."
    }
    if ($Output -match "hash does not match|0x8a150008|-1978335224|2316632072") {
        return "winget found that the download changed since its package list was made (it happens to the Build Tools after Microsoft updates them)."
    }
    if ($Code -in 3010, 1641) { return "installed: Windows has to restart before it can be used." }
    if ($Output -match "already installed|No newer package versions|0x8a15002b|-1978335189|2316632107") {
        return "winget says it is already installed, yet the setup does not find a working one (for Python: 3.11 or newer with pip)."
    }
    if ($Code -eq 8006) { return "another Visual Studio installer was busy: close it (or restart the PC) and run the setup again." }
    if (Test-WingetSourceBroken $Output) { return (Get-WingetSourceText) }
    # (TODO 443) Visual Studio's installer's documented exit codes, and the Windows errors installers pass on
    # (learn.microsoft.com: "Use command-line parameters to install Visual Studio", exit codes)
    switch ($Code) {
        740 { return "the installer needs administrator rights and did not get them. Run the setup again and click Yes when Windows asks; on a work or school PC an administrator may have to install it." }
        { $_ -in 1001, 1003 } { return "Visual Studio or its installer was already open. Close every Visual Studio window and its installer (or restart the PC), then run the setup again." }
        1618 { return "another program was installing at the same time (Windows Update or another installer). Wait for it to finish (or restart the PC), then run the setup again." }
        { $_ -in 5003, -1073720687, -2147012867, -2147012889, -2147012894 } { return "the installer could not download its files. Check the internet connection (a VPN, proxy or firewall can block Microsoft's download servers), then run the setup again: finished steps are kept." }
        { $_ -in 5007, 8010 } { return "Microsoft's installer says this PC does not meet the Build Tools' requirements (64-bit Windows 10 or 11 with its updates). Install Windows' updates, then run the setup again." }
        8004 { return "the installer could not use its install folder. Check that $env:SystemDrive has space and that no antivirus or policy blocks 'Program Files (x86)\Microsoft Visual Studio', then run the setup again." }
        { $_ -in 8005, 8002 } { return "the installer's downloaded files did not check out (a cut download, or a proxy changing it). Run the setup again; if it happens again, restart the PC first." }
        -1073741510 { return "the installer was closed before it finished (by a person, a restart or another program). Run the setup again: finished steps are kept." }
        -2147024891 { return "Windows refused access (an antivirus or a company policy blocking the installer). Allow it, or install the tools by hand with the link below." }
    }
    return ""
}

# Runs one install command line through cmd, its output shown and kept. Returns @{ Code; Output }. (A function of its
# own so the simulation test, tests\setup_prerequisites_tests.ps1, can stand in for winget.)
function Invoke-PrerequisiteCommand([string]$CommandLine) {
    $lines = New-Object System.Collections.Generic.List[string]
    $eap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & cmd /c $CommandLine 2>&1 | ForEach-Object { $s = "$_"; Write-Host $s; $lines.Add($s) } } finally { $ErrorActionPreference = $eap }
    return @{ Code = $LASTEXITCODE; Output = ($lines -join "`n") }
}

# A tool's own installer, downloaded from its maker and checked (its Authenticode signature: valid and signed by the
# maker) - when winget is missing or cannot do it: the Visual Studio Build Tools' bootstrapper (Microsoft's permanent
# link; with -Path it MODIFIES that install - and repairs a broken Visual Studio Installer on the way) and Python
# 3.12.10 from python.org (for this user only: no administrator rights). Returns @{ Code; Output }.
function Invoke-DirectInstall([string]$Tool, [string]$Path = "") {
    $arm = ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64")
    if ($Tool -eq "vs") {
        $url = "https://aka.ms/vs/17/release/vs_BuildTools.exe"; $signer = "Microsoft Corporation"; $file = "vs_BuildTools.exe"
        $add = @("--add", "Microsoft.VisualStudio.Workload.VCTools", "--includeRecommended", "--passive", "--wait", "--norestart")
        $argList = if ($Path) { @("modify", "--installPath", "`"$Path`"") + $add } else { $add }
        $elevate = $true
    } elseif ($Tool -eq "python") {
        $v = "3.12.10"
        $cpu = if ($arm) { "arm64" } else { "amd64" }
        $url = "https://www.python.org/ftp/python/$v/python-$v-$cpu.exe"; $signer = "Python Software Foundation"; $file = "python-$v.exe"
        $argList = @("/quiet", "InstallAllUsers=0", "PrependPath=1", "Include_pip=1", "Include_launcher=1", "InstallLauncherAllUsers=0")
        $elevate = $false
    } else { return @{ Code = -1; Output = "no direct installer for $Tool" } }
    $dest = Join-Path $env:TEMP "tfa_$file"
    try {
        Write-Host "Downloading $url"
        $pp = $ProgressPreference; $ProgressPreference = "SilentlyContinue"
        try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $dest } finally { $ProgressPreference = $pp }
        $sig = Get-AuthenticodeSignature -LiteralPath $dest
        if ($sig.Status -ne "Valid" -or "$($sig.SignerCertificate.Subject)" -notmatch [regex]::Escape($signer)) {
            Remove-Item -LiteralPath $dest -ErrorAction SilentlyContinue
            return @{ Code = -1; Output = "the download from $url is not signed by $signer ($($sig.Status)): not run" }
        }
        $ask = if ($elevate) { " - Windows asks for permission: click Yes" } else { "" }
        Write-Host "Running $file (signed by $signer)$ask"
        $sp = @{ FilePath = $dest; ArgumentList = $argList; Wait = $true; PassThru = $true }
        if ($elevate) { $sp.Verb = "RunAs" }
        $p = Start-Process @sp
        return @{ Code = $p.ExitCode; Output = "Installer failed with exit code: $($p.ExitCode)" }
    } catch {
        $declined = ($_.Exception.NativeErrorCode -eq 1223 -or $_.Exception.Message -match "canceled by the user|cancelled by the user")
        if ($declined) { return @{ Code = 1223; Output = "$($_.Exception.Message)`nInstaller failed with exit code: 1223" } }
        return @{ Code = -1; Output = "$($_.Exception.Message)" }
    }
}

# The missing prerequisites installed, one at a time, each checked again right after its install (PATH as the registry
# has it now). Returns @{ Still = names still missing; Report = lines to show; Restart = a restart is needed;
# Retry = a second run of the setup can help (the installers said they succeeded) }.
function Install-MissingPrerequisites([string[]]$Missing) {
    $how = Get-PrerequisiteInstall
    $manual = Get-PrerequisiteManualSteps
    $report = New-Object System.Collections.Generic.List[string]
    $still = @()
    $restart = $false
    $retry = $false
    $wingetProblem = Get-WingetProblem
    # (TODO 438) winget's source: "" until a winget install says it is damaged; then "repaired" (reset done, the install
    # tried once more), "declined" (Windows' permission not given) or "failed" (the reset did not help). Once is enough
    # for all the tools: Git, Python and the Build Tools share the one source.
    $sourceState = ""
    $arm = ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64")
    if ($wingetProblem) {
        Write-Host "warning: $wingetProblem - the setup downloads the tools from their makers instead." -ForegroundColor Yellow
    }
    foreach ($m in $Missing) {
        $code = $null
        $out = ""
        $gone = { Update-PathFromRegistry; @(Get-MissingPrerequisites) -notcontains $m }
        # 1. What is there but not usable, repaired in place (winget would only say "already installed"):
        if ($m -eq "python" -and (Repair-PythonPip)) {
            if (& $gone) { continue }
        }
        $fix = @()
        if ($m -eq "vs") { $fix = @(Get-VsToRepair) }
        if ($fix.Count -gt 0) {
            # Visual Studio already there without its C++ tools (or with them but no SDK, or an incomplete install):
            # its own installer adds them or finishes it (issue #2).
            $what = if ($fix[0].Action -eq "resume") { "Finishing the incomplete Visual Studio install" } else { "Adding the C++ tools and the Windows SDK to" }
            Write-Host "`n==> $what $($fix[0].Path) (Windows asks for permission: click Yes)" -ForegroundColor Cyan
            $code = Install-VsCppTools $fix[0].Path $fix[0].Action
            Write-Host "Visual Studio's installer finished (exit code $(Format-ExitCode $code))."
            $out = "Installer failed with exit code: $code"
            if ($code -eq -1) {
                # (its installer is missing or broken: Microsoft's bootstrapper repairs it while it modifies the install)
                $r = Invoke-DirectInstall "vs" $fix[0].Path
                $code = $r.Code; $out = $r.Output
            }
            if (& $gone) { continue }
        }
        # 2. winget, else the maker's own installer:
        # (TODO 438: not again once its source is known to stay damaged: the makers' installers, else the steps by hand)
        if (-not $wingetProblem -and $fix.Count -eq 0 -and $sourceState -notin "declined", "failed") {
            Write-Host "`n==> Installing $($how[$m][0])" -ForegroundColor Cyan
            if ($m -eq "vs") { Write-Host "(a few GB: its installer shows its progress; Windows asks for permission first - click Yes)" }
            $r = Invoke-PrerequisiteCommand $how[$m][1]
            $code = $r.Code
            $out = $r.Output
            if (& $gone) { continue }
            # (TODO 438) winget's source damaged: repaired once (Windows asks for permission), then this install once more.
            # (a source still damaged after an earlier tool's repair: no second repair, no second try)
            if ((Test-WingetSourceBroken "$out") -and $sourceState -eq "repaired") { $sourceState = "failed" }
            if ((Test-WingetSourceBroken "$out") -and $sourceState -eq "") {
                $rs = Repair-WingetSource
                $sourceState = if ($rs.Code -eq 1223) { "declined" } elseif ($rs.Code -eq 0) { "repaired" } else { "failed" }
                if ($sourceState -eq "repaired") {
                    Write-Host "`n==> Installing $($how[$m][0]) (once more, after the repair)" -ForegroundColor Cyan
                    $r = Invoke-PrerequisiteCommand $how[$m][1]
                    $code = $r.Code
                    $out = $r.Output
                    if (& $gone) { continue }
                    if ((Test-WingetSourceBroken "$out")) { $sourceState = "failed" }
                }
            }
            if ($m -eq "python" -and (Repair-PythonPip) -and (& $gone)) { continue }
            if ($m -eq "vs") {
                # (winget: "already installed" - an install vswhere did not list a moment ago may be listed now)
                $fix = @(Get-VsToRepair)
                if ($fix.Count -gt 0) {
                    $code = Install-VsCppTools $fix[0].Path $fix[0].Action
                    $out = "Installer failed with exit code: $code"
                    if (& $gone) { continue }
                }
            }
        }
        $inner = [regex]::Match("$out", "Installer failed with exit code: (-?\d+)").Groups[1].Value
        $permission = ("$out" -match "You cancelled the installation") -or ($code -in 1223, 1602, 5004) -or ($inner -in "2", "1223", "1602", "5004")
        $sourceBroken = ($sourceState -in "declined", "failed") -or ($sourceState -eq "" -and (Test-WingetSourceBroken "$out"))
        if (($wingetProblem -or $sourceBroken -or "$out" -match "hash does not match|already installed|No newer package versions") -and -not $permission -and $m -in "vs", "python") {
            Write-Host "`n==> Installing $($how[$m][0]) with its maker's own installer" -ForegroundColor Cyan
            $vsDir = ""
            if ($m -eq "vs") {
                $vsDir = @(Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName) | Select-Object -First 1
            }
            $r = Invoke-DirectInstall $m $vsDir
            $code = $r.Code; $out = "$out`n$($r.Output)"
            if (& $gone) { continue }
        }
        $still += $m
        if ($null -eq $code) {
            $why = if ($sourceBroken) { "winget's package list is damaged" } else { $wingetProblem }
            $report.Add("- $($how[$m][0]): not installed ($why).")
        } else {
            $meaning = Get-InstallFailureMeaning $code $out
            if ($code -eq 0 -and -not $meaning) {
                # (the installer says it worked and the tool is still not seen: a new PowerShell, or a restart, sees it)
                $retry = $true
                $meaning = "its installer finished without an error, but the setup does not see it yet."
            }
            if (-not $meaning) { $meaning = "its installer stopped with exit code $(Format-ExitCode (Get-InstallerExitCode $code $out)) (the lines above say why)." }
            if ($meaning -like "installed: Windows has to restart*") { $restart = $true }
            $report.Add("- $($how[$m][0]): $meaning")
        }
        if ($m -eq "python") { $report.Add("  What the setup found: $(Get-PythonFindings)") }
        if ($m -eq "vs") { $report.Add("  What the setup found: $(Get-VsFindings)") }
        if ($m -eq "vs" -and $arm) { $report.Add("  (This PC has an ARM processor: the game's build is made for x64 PCs and is not tested on ARM.)") }
        if ($wingetProblem) { $report.Add("  (${wingetProblem}: $($manual.winget))") }
        if ($sourceState -eq "declined") { $report.Add("  (The setup tried to repair winget's package list, but Windows' permission was not given: run the setup again and click Yes, or repair it by hand as below.)") }
        if ($sourceBroken -and -not (Test-WingetSourceBroken "$out")) { $report.Add("  ($(Get-WingetSourceText))") }
        $report.Add("  What to do: $($manual[$m])")
    }
    return @{ Still = $still; Report = @($report); Restart = $restart; Retry = $retry }
}

# Where this game folder sits in its git repository: "" when it is the repository's top (a clone of
# the game's repository), "port/" and the like when a repository holds it as a subfolder.
function Get-PortPrefix([string]$Root, [string]$Port) {
    $r = (Resolve-Path $Root).Path.TrimEnd('\')
    $p = (Resolve-Path $Port).Path.TrimEnd('\')
    if ($p -ieq $r) { return "" }
    if (-not $p.StartsWith($r + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "$Port is not inside $Root" }
    return $p.Substring($r.Length + 1).Replace('\', '/') + "/"
}

# What a hook script's Invoke-SetupHook / Invoke-UpdateHook get. A hook throws to stop with a message.
function New-HookContext($Root, $Port, $Python, $State, $HookArgs, $Changed = @(), [bool]$Force = $false) {
    [pscustomobject]@{
        Root = $Root; Port = $Port; Prefix = (Get-PortPrefix $Root $Port); Python = $Python
        State = $State; Args = $HookArgs; Changed = @($Changed); Force = $Force
    }
}

# Long paths for every git command, the build's own downloads included: a dependency's test files
# (toml11's doctest) pass Windows' 260-character limit in a nested install folder. And the install's own folders as
# safe git directories: on a drive that records no file owners (exFAT, FAT32 - a player's E: drive) git refuses them
# as "dubious ownership" and the build stops (only for this script's own git commands, nothing global is changed).
$env:GIT_CONFIG_COUNT = "2"; $env:GIT_CONFIG_KEY_0 = "core.longpaths"; $env:GIT_CONFIG_VALUE_0 = "true"
$env:GIT_CONFIG_KEY_1 = "safe.directory"; $env:GIT_CONFIG_VALUE_1 = "*"

# A "Transformers Armada" shortcut on the desktop, made once (the setup's own code does the same after a first
# install; this covers installs whose setup copy is older than that). Deleted by the player, it stays deleted: the
# mark in %LOCALAPPDATA% is shared with the setup. It starts the install folder's tfa-setup.exe (the game when the
# install is finished) with the game's icon, else the game's exe itself.
function Add-DesktopShortcutOnce([string]$Repo, [string]$Port) {
    if ($env:RECOMPLAUNCHER -eq '1') { return }
    try {
        $markDir = Join-Path $env:LOCALAPPDATA "TransformersArmadaSetup"
        $mark = Join-Path $markDir "desktop_shortcut_made"
        if (Test-Path $mark) { return }
        $exe = Join-Path $Port "out\build\transformers\RelWithDebInfo\TransformersRecomp.exe"
        if (-not (Test-Path $exe)) { return }
        $setupExe = Join-Path (Split-Path $Repo -Parent) "tfa-setup.exe"
        $target = if (Test-Path $setupExe) { $setupExe } else { $exe }
        $desk = [Environment]::GetFolderPath("Desktop")
        $s = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $desk "Transformers Armada.lnk"))
        $s.TargetPath = $target
        $s.WorkingDirectory = Split-Path $target -Parent
        $s.IconLocation = "$exe,0"
        $s.Description = "Transformers Armada (PC)"
        $s.Save()
        New-Item -ItemType Directory -Force $markDir | Out-Null
        Set-Content $mark ""
        Write-Host "A desktop shortcut was made: Transformers Armada."
    } catch {
        Write-Host "warning: the desktop shortcut could not be made: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}
