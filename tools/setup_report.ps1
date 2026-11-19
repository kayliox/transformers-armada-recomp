<#
.SYNOPSIS
    (build robustness) One text file with everything needed to help a player whose setup or update stopped: the step
    and its error, the PC (Windows, free space, the folder's drive, Smart App Control, antivirus), the tools' versions
    (Git, Python, CMake, every Visual Studio and its C++ tools), the build's state, and the ends of the logs.

.DESCRIPTION
    New-SetupReport -Port <the game's folder> [-What setup|update] [-Message <text>] writes tfa-report-<date>-<time>.txt
    to the Desktop (else the install's logs\ folder) and returns its path. setup.ps1 and update.ps1 call it when they
    stop, and say where it is. Holds no game files and nothing from the disc; the user's name, the user folder and the
    PC's name are replaced by <user>, <home> and <pc> everywhere in it.
    Standalone: powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup_report.ps1   (prints the file's path)

    Deps: Windows PowerShell 5.1+.
#>
# (no param() block: dot-sourced by setup.ps1 and update.ps1)

function Protect-ReportText([string]$Text) {
    if (-not $Text) { return $Text }
    $t = $Text
    foreach ($p in @($env:USERPROFILE, $env:HOMEPATH) | Where-Object { $_ } | Sort-Object Length -Descending) {
        $t = $t.Replace($p, "<home>").Replace($p.Replace('\', '/'), "<home>")
    }
    if ($env:USERNAME -and $env:USERNAME.Length -ge 2) { $t = [regex]::Replace($t, "(?i)(?<![A-Za-z0-9])" + [regex]::Escape($env:USERNAME) + "(?![A-Za-z0-9])", "<user>") }
    if ($env:COMPUTERNAME -and $env:COMPUTERNAME.Length -ge 2) { $t = [regex]::Replace($t, "(?i)(?<![A-Za-z0-9])" + [regex]::Escape($env:COMPUTERNAME) + "(?![A-Za-z0-9])", "<pc>") }
    return $t
}

# The last lines of a file another program may still be writing (read with sharing on).
function Get-FileTail([string]$Path, [int]$Lines = 200) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try {
        $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            $max = 4MB
            if ($fs.Length -gt $max) { $fs.Seek(-$max, [IO.SeekOrigin]::End) | Out-Null }
            $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
        } finally { $fs.Dispose() }
        $all = $text -split "`r?`n"
        return (@($all | Select-Object -Last $Lines) -join "`n")
    } catch { return "(could not be read: $($_.Exception.Message))" }
}

function Get-ToolLine([scriptblock]$Block) {
    try { $ErrorActionPreference = "Continue"; return "$(& $Block 2>&1 | Select-Object -First 1)".Trim() } catch { return "(not found)" }
}

# Where the report went, for the error line: "on your Desktop as <name>" (the setup's error is often shared as a
# screenshot: no user folder in it), else the path with the user folder taken out.
function Get-ReportPlace([string]$Path) {
    $desk = [Environment]::GetFolderPath("Desktop")
    if ($desk -and (Split-Path $Path -Parent) -eq $desk) { return "on your Desktop as $(Split-Path $Path -Leaf)" }
    return "as $(Protect-ReportText $Path)"
}

function New-SetupReport {
    param([string]$Port, [string]$What = "setup", [string]$Message = "")
    $s = [Text.StringBuilder]::new()
    function Line([string]$t = "") { [void]$s.AppendLine($t) }
    function Section([string]$t) { Line; Line "=== $t ===" }
    $Port = try { (Resolve-Path -LiteralPath $Port).Path } catch { $Port }
    $version = try { "$(Get-Content -LiteralPath (Join-Path $Port 'VERSION') -Raw)".Trim() } catch { "?" }
    $commit = Get-ToolLine { git -C $Port rev-parse --short HEAD }
    Line "Transformers Armada PC version: $What report, $(Get-Date -Format 'yyyy-MM-dd HH:mm') (version $version, commit $commit)"
    Line "This file holds no game files; the user name, the user folder and the PC's name are replaced by <user>, <home>, <pc>."
    if ($Message) { Section "where it stopped"; Line $Message }

    Section "this PC"
    try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop; Line "Windows: $($os.Caption) $($os.Version) (build $($os.BuildNumber)), $env:PROCESSOR_ARCHITECTURE" } catch { Line "Windows: $([Environment]::OSVersion.VersionString)" }
    Line "PowerShell: $($PSVersionTable.PSVersion)"
    try { $cs = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop; Line "Processor threads: $([Environment]::ProcessorCount), memory: $([math]::Round($cs.TotalPhysicalMemory / 1GB, 1)) GB" } catch {}
    Line "Game's folder: $Port"
    foreach ($d in @([IO.Path]::GetPathRoot($Port), "$env:SystemDrive\") | Select-Object -Unique) {
        try { $di = [IO.DriveInfo]::new($d); Line "Drive $($d): $([math]::Round($di.AvailableFreeSpace / 1GB, 1)) GB free of $([math]::Round($di.TotalSize / 1GB)) GB, $($di.DriveFormat), $($di.DriveType)" } catch {}
    }
    try { Line "Windows' long paths: $(if ((Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled -ErrorAction Stop).LongPathsEnabled -eq 1) { 'on' } else { 'off' })" } catch { Line "Windows' long paths: off" }
    try { $sac = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy" -Name VerifiedAndReputablePolicyState -ErrorAction Stop).VerifiedAndReputablePolicyState; Line "Smart App Control: $(@{0='off';1='ON';2='evaluation'}[[int]$sac])" } catch { Line "Smart App Control: not present" }
    try { Line "Antivirus: $((@(Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction Stop | ForEach-Object displayName | Select-Object -Unique)) -join ', ')" } catch {}
    try {
        $gpus = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" })
        Line "Graphics: $($gpus -join '; ')"
    } catch {}

    Section "the tools"
    Line "git: $(Get-ToolLine { git --version }) at $((Get-Command git -ErrorAction SilentlyContinue).Source)"
    Line "python: $(Get-ToolLine { python --version }) at $((Get-Command python -ErrorAction SilentlyContinue).Source)"
    Line "cmake: $(Get-ToolLine { cmake --version }) at $((Get-Command cmake -ErrorAction SilentlyContinue).Source)"
    Line "winget: $(Get-ToolLine { winget --version })"
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        try {
            $all = @("$(& $vswhere -all -prerelease -products * -format json 2>$null | Out-String)" | ConvertFrom-Json | ForEach-Object { $_ })
            $cpp = @("$(& $vswhere -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Out-String)" -split "`r?`n" | Where-Object { $_ })
            foreach ($v in $all) {
                $msvc = @(Get-ChildItem -LiteralPath (Join-Path $v.installationPath "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)
                Line "Visual Studio: $($v.displayName) $($v.catalog.productDisplayVersion) at $($v.installationPath); C++ tools: $(if ($cpp -contains $v.installationPath) { 'yes' } else { 'NO' }); MSVC toolsets: $(if ($msvc) { $msvc -join ', ' } else { 'none' })"
            }
            if (-not $all) { Line "Visual Studio: none (vswhere lists nothing)" }
        } catch { Line "Visual Studio: vswhere failed ($($_.Exception.Message))" }
    } else { Line "Visual Studio: no installer on this PC (vswhere.exe missing)" }
    $sdks = @(Get-ChildItem -LiteralPath "${env:ProgramFiles(x86)}\Windows Kits\10\Include" -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)
    Line "Windows SDKs: $(if ($sdks) { $sdks -join ', ' } else { 'none' })"

    if (Get-Command Invoke-Preflight -ErrorAction SilentlyContinue) {
        Section "the checks before the install"
        try { foreach ($f in Invoke-Preflight -Port $Port -Update:($What -eq "update")) { Line "$($f.Level): $($f.Text)" } } catch { Line "(failed: $($_.Exception.Message))" }
    }

    Section "the build's state"
    $patchDir = Join-Path $Port "patches\ps2recomp"
    $series = @(Get-ChildItem -LiteralPath $patchDir -Filter *.patch -ErrorAction SilentlyContinue).Count
    $rec = Join-Path $Port "out\ps2recomp-patches.txt"
    $recorded = if (Test-Path -LiteralPath $rec) { @(Get-Content -LiteralPath $rec | Where-Object { $_ }).Count } else { 0 }
    Line "PS2Recomp: $(Get-ToolLine { git -C (Join-Path $Port 'PS2Recomp') rev-parse --short HEAD }); patches in the series: $series, applied (recorded): $recorded"
    foreach ($f in "out\tools\ps2xRecomp\RelWithDebInfo\ps2_recomp.exe", "out\tools\ps2xAnalyzer\RelWithDebInfo\ps2_analyzer.exe",
                   "work\generated\register_functions.cpp", "out\build\transformers\RelWithDebInfo\TransformersRecomp.exe",
                   "out\build-public\transformers\RelWithDebInfo\TransformersRecomp.exe") {
        $p = Join-Path $Port $f
        Line "$f : $(if (Test-Path -LiteralPath $p) { $i = Get-Item -LiteralPath $p; "$([math]::Round($i.Length / 1MB, 1)) MB, $($i.LastWriteTime.ToString('yyyy-MM-dd HH:mm'))" } else { 'missing' })"
    }
    foreach ($c in "out\tools\CMakeCache.txt", "out\build\CMakeCache.txt", "out\build-public\CMakeCache.txt") {
        $p = Join-Path $Port $c
        if (Test-Path -LiteralPath $p) {
            $keys = @(Select-String -LiteralPath $p -Pattern '^(CMAKE_GENERATOR|CMAKE_GENERATOR_INSTANCE|CMAKE_GENERATOR_TOOLSET|CMAKE_CACHE_MAJOR_VERSION|CMAKE_CACHE_MINOR_VERSION|CMAKE_SYSTEM_VERSION|FETCHCONTENT_SOURCE_DIR_[A-Z0-9_]+)(:[A-Z]+)?=' | ForEach-Object { $_.Line })
            Line "$c : $($keys -join ' | ')"
        }
    }
    $deps = Join-Path $Port "out\deps"
    if (Test-Path -LiteralPath $deps) { Line "out\deps: $((@(Get-ChildItem -LiteralPath $deps -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName '.tfa_dep') } | ForEach-Object Name)) -join ', ')" }
    $times = Join-Path $Port "out\build-times.json"
    if (Test-Path -LiteralPath $times) { Line "build times: $((Get-Content -LiteralPath $times -Raw) -replace '\s+', ' ')" }

    # The logs: the step record whole (small), the others' ends.
    $logsDir = try { Get-LayoutLogsDir $Port } catch { "" }
    $places = @($Port, (Split-Path $Port -Parent)) + @($(if ($logsDir) { $logsDir }))
    foreach ($name in @("setup_steps.log", "update_steps.log", "setup.log", "update-setup.log", "update.log")) {
        $file = $places | ForEach-Object { Join-Path $_ $name } | Where-Object { Test-Path -LiteralPath $_ } |
            Sort-Object { (Get-Item -LiteralPath $_).LastWriteTime } -Descending | Select-Object -First 1
        if ($file) {
            Section "$name (the last lines, $((Get-Item -LiteralPath $file).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))"
            Line (Get-FileTail $file $(if ($name -like "*steps*") { 400 } else { 300 }))
        }
    }
    foreach ($w in @(@("work\analyzer.log", 30), @("work\recomp.log", 60))) {
        $p = Join-Path $Port $w[0]
        if (Test-Path -LiteralPath $p) { Section "$($w[0]) (the last lines)"; Line (Get-FileTail $p $w[1]) }
    }

    $text = Protect-ReportText $s.ToString()
    $name = "tfa-report-$(Get-Date -Format 'yyyyMMdd-HHmmss').txt"
    # (TF_REPORT_DIR: another folder first - the tests)
    $dirs = @($(if ($env:TF_REPORT_DIR) { $env:TF_REPORT_DIR })) + @([Environment]::GetFolderPath("Desktop")) + @($(if ($logsDir) { $logsDir })) + @($Port)
    foreach ($dir in $dirs | Where-Object { $_ -and (Test-Path -LiteralPath $_) }) {
        try {
            $path = Join-Path $dir $name
            [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
            return $path
        } catch {}
    }
    return $null
}

# Run on its own: write the report, print where it is.
if ($MyInvocation.InvocationName -ne '.') {
    $here = Split-Path $PSScriptRoot -Parent
    foreach ($f in "install_layout.ps1", "preflight.ps1") { $p = Join-Path $PSScriptRoot $f; if (Test-Path -LiteralPath $p) { . $p } }
    $path = New-SetupReport -Port $here -What "setup" -Message "$($args -join " ")"
    if ($path) { Write-Host "The report: $path" } else { Write-Host "The report could not be written."; exit 1 }
}
