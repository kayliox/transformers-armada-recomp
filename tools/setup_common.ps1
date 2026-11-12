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
    return @($c | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)
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
    if ($found.Count -eq 0) { return "no python.exe found (not on PATH, not in the py launcher, the usual folders or the registry)" }
    return "found: " + ($found -join "; ")
}

function Use-Python([string]$Exe) {
    $dir = Split-Path $Exe
    $userScripts = Invoke-Probe { & $Exe -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))" }
    $env:PATH = "$dir;$dir\Scripts;$userScripts;$env:PATH"
}

# Fonts improve prompts, but must not prevent an otherwise playable install or its mods.
function Invoke-OptionalFonts([string]$Python, [string]$Port) {
    try {
        Invoke-Probe { & $Python -c "import numpy" } | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Host "`n==> Installing numpy (button prompts and fonts)" -ForegroundColor Cyan
            $eap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try { & $Python -m pip install --user numpy 2>&1 | ForEach-Object { "$_" } }
            finally { $ErrorActionPreference = $eap }
            if ($LASTEXITCODE -ne 0) { throw "pip install numpy failed" }
        }
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

# What the build needs and is not there: "git", "python", "vs" (Visual Studio's C++ tools).
function Get-MissingPrerequisites {
    $m = @()
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { $m += "git" }
    if (-not (Find-Python)) { $m += "python" }   # (a real one: not Windows' Microsoft Store shortcut)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = (Test-Path $vswhere) -and [bool](Invoke-Probe { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath })
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

# The installs as objects: @{ Path; Complete; Cpp } (vswhere -all; its JSON says whether each is complete).
function Get-VsInstances {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return @() }
    $json = "$(Invoke-Probe { & $vswhere -all -products * -format json } | Out-String)"
    $cpp = @(Invoke-Probe { & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } | Where-Object { $_ })
    $list = @()
    try { $list = @($json | ConvertFrom-Json | ForEach-Object { $_ }) } catch { }
    return @($list | Where-Object { $_.installationPath } | ForEach-Object {
        [pscustomobject]@{ Path = $_.installationPath; Complete = ($_.isComplete -ne $false); Cpp = ($cpp -contains $_.installationPath) } })
}

# The installs to repair, best first: @{ Path; Action = "resume" (finish an incomplete one) or "modify" (add the C++ tools) }.
function Get-VsToRepair {
    $all = @(Get-VsInstances)
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
    return "found: " + (($i | ForEach-Object { "$($_.Path) (" + $(if (-not $_.Complete) { "incomplete" } elseif ($_.Cpp) { "with the C++ tools" } else { "WITHOUT the C++ tools" }) + ")" }) -join "; ") + "; $sdk"
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
        vs     = "Visual Studio Build Tools: download them from https://visualstudio.microsoft.com/visual-cpp-build-tools/ , run them, tick 'Desktop development with C++' (keep the Windows 11 SDK ticked on the right) and press Install (about 7 GB). Already have Visual Studio or its Build Tools? Open 'Visual Studio Installer' from the Start menu, press Modify and tick the same 'Desktop development with C++'."
        winget = "winget (Windows' package manager) is missing or too old: install or update 'App Installer' from the Microsoft Store, https://apps.microsoft.com/detail/9NBLGGH4NNS1 , then run the setup again. (Windows Server and LTSC editions have no Store: install the tools by hand with the links below.)"
    }
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

# What an installer's exit code and last lines mean, in words a player can act on ("" when nothing is known).
function Get-InstallFailureMeaning([int]$Code, [string]$Output) {
    # (winget's own exit code says only "the installer failed"; the installer's is in its output)
    $m = [regex]::Match($Output, "Installer failed with exit code: (-?\d+)")
    if ($m.Success) { $Code = [int]$m.Groups[1].Value }
    if ($Output -match "You cancelled the installation" -or $Code -in 2, 1223, 1602, 5004) {
        # (2: Git's installer (Inno Setup) when Windows' question was declined or closed itself unanswered - the ThinkPad test)
        return "Windows asked for permission to install and nobody answered (or No was chosen). The question may show only as a flashing shield button in the taskbar, and it closes by itself after about 2 minutes. Run the setup again and click Yes."
    }
    if ($Output -match "hash does not match|0x8a150008|-1978335224") {
        return "winget found that the download changed since its package list was made (it happens to the Build Tools after Microsoft updates them)."
    }
    if ($Code -in 3010, 1641) { return "installed: Windows has to restart before it can be used." }
    if ($Output -match "already installed|No newer package versions|0x8a15002b|-1978335189") {
        return "winget says it is already installed, yet the setup does not find a working one (for Python: 3.11 or newer with pip)."
    }
    if ($Code -eq 8006) { return "another Visual Studio installer was busy: close it (or restart the PC) and run the setup again." }
    if ($Output -match "0x8a15000f|0x8a150044|Failed when searching source|Failed in attempting to update the source") { return "winget could not read its package list (no network, or its source is broken: 'winget source reset --force' in an administrator PowerShell repairs it)." }
    if ($Output -match "0x80070070|not enough (disk )?space|There is not enough space") { return "not enough free disk space (the Build Tools need about 7 GB on C:)." }
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
            Write-Host "Visual Studio's installer finished (exit code $code)."
            $out = "Installer failed with exit code: $code"
            if ($code -eq -1) {
                # (its installer is missing or broken: Microsoft's bootstrapper repairs it while it modifies the install)
                $r = Invoke-DirectInstall "vs" $fix[0].Path
                $code = $r.Code; $out = $r.Output
            }
            if (& $gone) { continue }
        }
        # 2. winget, else the maker's own installer:
        if (-not $wingetProblem -and $fix.Count -eq 0) {
            Write-Host "`n==> Installing $($how[$m][0])" -ForegroundColor Cyan
            if ($m -eq "vs") { Write-Host "(a few GB: its installer shows its progress; Windows asks for permission first - click Yes)" }
            $r = Invoke-PrerequisiteCommand $how[$m][1]
            $code = $r.Code
            $out = $r.Output
            if (& $gone) { continue }
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
        if (($wingetProblem -or "$out" -match "hash does not match|already installed|No newer package versions") -and -not $permission -and $m -in "vs", "python") {
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
            $report.Add("- $($how[$m][0]): not installed ($wingetProblem).")
        } else {
            $meaning = Get-InstallFailureMeaning $code $out
            if ($code -eq 0 -and -not $meaning) {
                # (the installer says it worked and the tool is still not seen: a new PowerShell, or a restart, sees it)
                $retry = $true
                $meaning = "its installer finished without an error, but the setup does not see it yet."
            }
            if (-not $meaning) { $meaning = "its installer stopped with exit code $code (the lines above say why)." }
            if ($meaning -like "installed: Windows has to restart*") { $restart = $true }
            $report.Add("- $($how[$m][0]): $meaning")
        }
        if ($m -eq "python") { $report.Add("  What the setup found: $(Get-PythonFindings)") }
        if ($m -eq "vs") { $report.Add("  What the setup found: $(Get-VsFindings)") }
        if ($m -eq "vs" -and $arm) { $report.Add("  (This PC has an ARM processor: the game's build is made for x64 PCs and is not tested on ARM.)") }
        if ($wingetProblem) { $report.Add("  (${wingetProblem}: $($manual.winget))") }
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
