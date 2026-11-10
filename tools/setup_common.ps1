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
    $candidates = @()
    $candidates += Get-Command python -All -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -and $_.Source -notlike "*\WindowsApps\*" } | ForEach-Object Source
    if (Get-Command py -ErrorAction SilentlyContinue) {
        $candidates += Invoke-Probe { & py -3 -c "import sys; print(sys.executable)" }
    }
    $candidates += Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe",
        "$env:ProgramFiles\Python3*\python.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | ForEach-Object FullName
    foreach ($exe in ($candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -Unique)) {
        # (3.11 or newer WITH pip: a Python bundled inside another program - Inkscape's, found on a player's PATH -
        # has no pip, and the build installs its tools with pip)
        Invoke-Probe { & $exe -c "import sys, pip; sys.exit(0 if sys.version_info >= (3, 11) else 1)" } | Out-Null
        if ($LASTEXITCODE -eq 0) { return $exe }
    }
    return $null
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

# Visual Studio installs (any edition, the Build Tools too) that LACK the C++ tools: their folders. winget sees such
# an install as "already installed" and adds nothing, so the setup kept stopping with "not found yet: vs" (issue #2,
# 2026-11-09). Install-VsCppTools adds the C++ tools to one of them with Visual Studio's own installer.
function Get-VsWithoutCpp {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return @() }
    $all = @(Invoke-Probe { & $vswhere -all -products * -property installationPath } | Where-Object { $_ })
    $cpp = @(Invoke-Probe { & $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } | Where-Object { $_ })
    $without = @($all | Where-Object { $cpp -notcontains $_ })
    # (the C++ tools there but no Windows SDK: the install with them is the one to modify - its recommended
    # components bring the SDK; it comes first)
    if ($cpp.Count -gt 0 -and -not (Test-WindowsSdk)) { return @($cpp + $without) }
    return $without
}

# Adds the C++ tools ("Desktop development with C++" / the VCTools workload) to an existing Visual Studio install.
# Windows asks for permission once (the installer needs admin rights). Returns the installer's exit code.
function Install-VsCppTools([string]$Path) {
    $installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\setup.exe"
    if (-not (Test-Path $installer)) { return -1 }
    $workload = if ((Split-Path $Path -Leaf) -eq "BuildTools") { "Microsoft.VisualStudio.Workload.VCTools" } else { "Microsoft.VisualStudio.Workload.NativeDesktop" }
    $a = @("modify", "--installPath", "`"$Path`"", "--add", $workload, "--includeRecommended", "--passive", "--norestart")
    try {
        $p = Start-Process -FilePath $installer -ArgumentList $a -Verb RunAs -Wait -PassThru
        return $p.ExitCode
    } catch {
        Write-Host "warning: Visual Studio's installer did not start: $($_.Exception.Message)" -ForegroundColor Yellow
        return -1
    }
}

# Each prerequisite's name and its winget command (-InstallPrerequisites runs them).
function Get-PrerequisiteInstall {
    $accept = "--accept-package-agreements --accept-source-agreements"
    @{
        git    = @("Git", "winget install --id Git.Git -e $accept")
        python = @("Python 3.11 or newer", "winget install --id Python.Python.3.12 -e $accept")
        vs     = @("Visual Studio 2022 Build Tools with the C++ tools",
                   "winget install --id Microsoft.VisualStudio.2022.BuildTools -e $accept --override `"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"")
    }
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
