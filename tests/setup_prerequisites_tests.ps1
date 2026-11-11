# setup_prerequisites_tests.ps1 - the setup's prerequisite step (Git, Python, Visual Studio's C++ tools) against simulated
# PCs: winget missing, Windows' permission question declined or timed out, a changed download, a restart needed, an
# installer that reports success but the tool is still not seen, winget's "already installed" for a Python without pip
# or a Build Tools install without the C++ tools / incomplete / not listed (two players' reports, 2026-11-11), Visual
# Studio's installer missing, an ARM PC. Nothing is installed: winget, vswhere, the makers' installers and the checks
# are stood in for. Two levels:
#   1. tools\setup_common.ps1's Install-MissingPrerequisites, scenario by scenario;
#   2. setup.ps1 itself, run in a child Windows PowerShell with the stand-ins given as its -Hooks script (dot-sourced
#      after setup_common.ps1, so they replace the real functions): the exact text a player sees.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\setup_prerequisites_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}

# The simulated PC: what is missing, what each install does (its exit code and output, and whether the tool is there
# afterwards), whether winget works, what vswhere lists, whether Python is there without pip.
$StandIns = @'
$global:SimMissing = [System.Collections.Generic.List[string]]::new()
foreach ($n in $env:SIM_MISSING.Split(',')) { if ($n) { $global:SimMissing.Add($n) } }
$global:SimCalls = [System.Collections.Generic.List[string]]::new()
$global:SimVsListed = [bool]$env:SIM_VS_NOCPP
function Get-WingetProblem { if ($env:SIM_WINGET) { return $env:SIM_WINGET } else { return $null } }
function Get-MissingPrerequisites { return @($global:SimMissing) }
function Update-PathFromRegistry { }
function Get-PythonFindings { return "sim: python findings" }
function Get-VsFindings { return "sim: vs findings" }
function Write-PrerequisiteEvidence([string]$When) { Write-Host "sim evidence ($When)" }
# (a Python 3.11+ there without pip: SIM_PY_NOPIP=fixable)
function Repair-PythonPip {
    if ($env:SIM_PY_NOPIP -eq "fixable" -and $global:SimMissing -contains "python") { $global:SimCalls.Add("ensurepip"); [void]$global:SimMissing.Remove("python"); return "C:\Py\python.exe" }
    return $null
}
# (an install vswhere lists: SIM_VS_NOCPP=1 from the start, SIM_VS_LATE=1 only after winget's attempt; SIM_VS_ACTION modify / resume)
function Get-VsToRepair {
    if ($global:SimVsListed -and $global:SimMissing -contains "vs") {
        $act = if ($env:SIM_VS_ACTION) { $env:SIM_VS_ACTION } else { "modify" }
        return @([pscustomobject]@{ Path = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"; Action = $act })
    }
    return @()
}
function Get-VsWithoutCpp { return @(Get-VsToRepair | ForEach-Object { $_.Path }) }
function Install-VsCppTools([string]$Path, [string]$Action = "modify") {
    $global:SimCalls.Add($Action)
    $code = [int]$env:SIM_VS_MODIFY
    if ($code -eq 0 -or $code -eq 3010) { [void]$global:SimMissing.Remove("vs") }
    return $code
}
# (the maker's own installer: SIM_DIRECT_<TOOL> = "<code>|<installed 0/1>"; default: it fails, 1603)
function Invoke-DirectInstall([string]$Tool, [string]$Path = "") {
    $global:SimCalls.Add("direct-$Tool")
    $spec = [Environment]::GetEnvironmentVariable("SIM_DIRECT_" + $Tool.ToUpper())
    if (-not $spec) { $spec = "1603|0" }
    $p = $spec.Split('|')
    if ($p[1] -eq "1") { [void]$global:SimMissing.Remove($Tool) }
    return @{ Code = [int]$p[0]; Output = "Installer failed with exit code: $($p[0])" }
}
function Invoke-PrerequisiteCommand([string]$CommandLine) {
    $tool = if ($CommandLine -match "Git\.Git") { "git" } elseif ($CommandLine -match "Python") { "python" } else { "vs" }
    $global:SimCalls.Add($tool)
    if ($tool -eq "vs" -and $env:SIM_VS_LATE) { $global:SimVsListed = $true }
    $spec = [Environment]::GetEnvironmentVariable("SIM_" + $tool.ToUpper())   # "<code>|<output>|<installed 0/1>"
    if (-not $spec) { $spec = "0|Successfully installed|1" }
    $p = $spec.Split('|')
    if ($p[2] -eq "1") { [void]$global:SimMissing.Remove($tool) }
    Write-Host $p[1].Replace('\n', "`n")
    return @{ Code = [int]$p[0]; Output = $p[1].Replace('\n', "`n") }
}
'@

# (winget's words on the second player's screenshot)
$already = "-1978335189|Found an existing package already installed. Trying to upgrade the installed package...\nNo available upgrade found.\nNo newer package versions are available from the configured sources.|0"
$scenarios = @(
    @{ Name = "everything installs"; Env = @{ SIM_MISSING = "git,python,vs" }
       Expect = { param($r) $r.Still.Count -eq 0 } },
    @{ Name = "no winget: Python and the Build Tools from their makers"; Env = @{ SIM_MISSING = "python,vs"; SIM_WINGET = "winget is not installed on this PC"; SIM_DIRECT_PYTHON = "0|1"; SIM_DIRECT_VS = "0|1" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "direct-python" -and $global:SimCalls -contains "direct-vs" -and $global:SimCalls -notcontains "vs" } },
    @{ Name = "no winget and the makers' installers fail: App Installer + every link"; Env = @{ SIM_MISSING = "python,vs"; SIM_WINGET = "winget is not installed on this PC" }
       Expect = { param($r) $r.Still.Count -eq 2 -and ($r.Report -join "`n") -match "App Installer" -and ($r.Report -join "`n") -match "python.org" -and
                  ($r.Report -join "`n") -match "visual-cpp-build-tools" } },
    @{ Name = "permission declined / timed out (Reddit player): no second try, click Yes"; Env = @{ SIM_MISSING = "python,vs"; SIM_PYTHON = "-1978335226|Installer failed with exit code: 2|0"; SIM_VS = "-1978335226|Installer failed with exit code: 1223|0" }
       Expect = { param($r) $r.Still.Count -eq 2 -and (($r.Report -join "`n") -split "click Yes").Count -eq 3 -and -not $r.Retry -and $global:SimCalls -notcontains "direct-vs" } },
    @{ Name = "Build Tools download changed (hash mismatch): Microsoft's bootstrapper instead"; Env = @{ SIM_MISSING = "vs"; SIM_VS = "-1978335215|Installer hash does not match; this cannot be overridden when running as admin|0"; SIM_DIRECT_VS = "0|1" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "direct-vs" } },
    @{ Name = "Build Tools installed, restart needed"; Env = @{ SIM_MISSING = "vs"; SIM_VS = "-1978335226|Installer failed with exit code: 3010|0" }
       Expect = { param($r) $r.Restart -and ($r.Report -join "`n") -match "restart" } },
    @{ Name = "installer says success, tool not seen yet (PATH)"; Env = @{ SIM_MISSING = "python"; SIM_PYTHON = "0|Successfully installed|0" }
       Expect = { param($r) $r.Retry -and ($r.Report -join "`n") -match "does not see it yet" } },
    @{ Name = "Discord player: a Python without pip -> ensurepip, no winget call"; Env = @{ SIM_MISSING = "python"; SIM_PY_NOPIP = "fixable" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "ensurepip" -and $global:SimCalls -notcontains "python" } },
    @{ Name = "Discord player: Python 'already installed', not repairable -> python.org installer"; Env = @{ SIM_MISSING = "python"; SIM_PYTHON = $already; SIM_DIRECT_PYTHON = "0|1" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "direct-python" } },
    @{ Name = "Discord player: an INCOMPLETE Build Tools install -> resume"; Env = @{ SIM_MISSING = "vs"; SIM_VS_NOCPP = "1"; SIM_VS_ACTION = "resume"; SIM_VS_MODIFY = "0" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "resume" -and $global:SimCalls -notcontains "vs" } },
    @{ Name = "Discord player: Build Tools 'already installed', listed after winget -> modify"; Env = @{ SIM_MISSING = "vs"; SIM_VS = $already; SIM_VS_LATE = "1"; SIM_VS_MODIFY = "0" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "vs" -and $global:SimCalls -contains "modify" } },
    @{ Name = "Discord player: Build Tools 'already installed', never listed -> bootstrapper modify"; Env = @{ SIM_MISSING = "vs"; SIM_VS = $already; SIM_DIRECT_VS = "0|1" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "direct-vs" } },
    @{ Name = "Discord player, nothing works: findings + Modify steps in the report"; Env = @{ SIM_MISSING = "python,vs"; SIM_PYTHON = $already; SIM_VS = $already }
       Expect = { param($r) $r.Still.Count -eq 2 -and ($r.Report -join "`n") -match "sim: python findings" -and ($r.Report -join "`n") -match "sim: vs findings" -and
                  ($r.Report -join "`n") -match "press Modify" -and ($r.Report -join "`n") -match "Add python.exe to PATH" } },
    @{ Name = "Visual Studio without C++: modify, permission declined"; Env = @{ SIM_MISSING = "vs"; SIM_VS_NOCPP = "1"; SIM_VS_MODIFY = "1223" }
       Expect = { param($r) $global:SimCalls -contains "modify" -and $global:SimCalls -notcontains "vs" -and ($r.Report -join "`n") -match "click Yes" } },
    @{ Name = "Visual Studio without C++: modify works"; Env = @{ SIM_MISSING = "vs"; SIM_VS_NOCPP = "1"; SIM_VS_MODIFY = "0" }
       Expect = { param($r) $r.Still.Count -eq 0 } },
    @{ Name = "Visual Studio's installer missing (modify -1): the bootstrapper repairs it"; Env = @{ SIM_MISSING = "vs"; SIM_VS_NOCPP = "1"; SIM_VS_MODIFY = "-1"; SIM_DIRECT_VS = "0|1" }
       Expect = { param($r) $r.Still.Count -eq 0 -and $global:SimCalls -contains "direct-vs" } },
    @{ Name = "winget's source broken"; Env = @{ SIM_MISSING = "git"; SIM_GIT = "-1978335217|Failed when searching source: winget\nAn unexpected error occurred while executing the command: 0x8a15000f|0" }
       Expect = { param($r) ($r.Report -join "`n") -match "source reset" -and ($r.Report -join "`n") -match "git-scm.com" } },
    @{ Name = "ARM64 PC"; Env = @{ SIM_MISSING = "vs"; SIM_VS = "1|Installer failed with exit code: 1|0"; PROCESSOR_ARCHITECTURE = "ARM64" }
       Expect = { param($r) ($r.Report -join "`n") -match "ARM" -and ($r.Report -join "`n") -match "exit code 1" } }
)

$simVars = @("SIM_MISSING", "SIM_WINGET", "SIM_GIT", "SIM_PYTHON", "SIM_VS", "SIM_VS_NOCPP", "SIM_VS_MODIFY", "SIM_VS_ACTION", "SIM_VS_LATE",
             "SIM_PY_NOPIP", "SIM_DIRECT_PYTHON", "SIM_DIRECT_VS")
$arch = $env:PROCESSOR_ARCHITECTURE

Write-Host "1. Install-MissingPrerequisites"
. (Join-Path $Root "tools\setup_common.ps1")
foreach ($s in $scenarios) {
    foreach ($v in $simVars) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
    $env:PROCESSOR_ARCHITECTURE = $arch
    foreach ($k in $s.Env.Keys) { Set-Item "Env:$k" $s.Env[$k] }
    Invoke-Expression $StandIns
    $r = Install-MissingPrerequisites @($global:SimMissing) 6>$null
    $ok = [bool](& $s.Expect $r)
    Check $ok $s.Name
    if (-not $ok) { Write-Host ("        calls: " + ($global:SimCalls -join ",") + "; still: " + ($r.Still -join ",") + "; report:`n        " + ($r.Report -join "`n        ")) }
}
$env:PROCESSOR_ARCHITECTURE = $arch

Write-Host "2. setup.ps1 (the text a player sees)"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("tfa_setup_sim_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $tmp | Out-Null
try {
    $hooks = Join-Path $tmp "standins.ps1"; Set-Content $hooks $StandIns
    $iso = Join-Path $tmp "disc.iso"; Set-Content $iso "x"
    # (a copy of the game folder's scripts: setup.ps1 writes .setup.json beside itself)
    $copy = Join-Path $tmp "port"; New-Item -ItemType Directory "$copy\tools" | Out-Null
    Copy-Item (Join-Path $Root "setup.ps1") $copy; Copy-Item (Join-Path $Root "tools\setup_common.ps1") "$copy\tools"
    $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"   # (the setup program's own PowerShell)
    $cases = @(
        @{ Name = "permission declined: says click Yes, no retry phrase"; Env = @{ SIM_MISSING = "vs"; SIM_VS = "-1978335226|You cancelled the installation.\nInstaller failed with exit code: 2|0" }
           Match = @("still missing: Visual Studio 2022 Build Tools", "click Yes", "nobody answered", "visual-cpp-build-tools", "run the setup again"); NotMatch = @("prerequisites installed") },
        @{ Name = "no winget, makers' installers fail: App Installer and every link"; Env = @{ SIM_MISSING = "git,python,vs"; SIM_WINGET = "winget is not installed on this PC" }
           Match = @("App Installer", "git-scm.com", "python.org", "Add python.exe to PATH", "Desktop development with C\+\+"); NotMatch = @("prerequisites installed") },
        @{ Name = "second player's screen ('already installed' x2): findings + Modify, no retry phrase"; Env = @{ SIM_MISSING = "python,vs"; SIM_PYTHON = $already; SIM_VS = $already }
           Match = @("still missing: Python 3.11 or newer, Visual Studio", "sim evidence \(after the installs\)", "What the setup found", "press Modify", "Add python.exe to PATH"); NotMatch = @("prerequisites installed") },
        @{ Name = "success but unseen: the retry phrase tfa-setup.exe looks for"; Env = @{ SIM_MISSING = "vs"; SIM_VS = "0|Successfully installed|0" }
           Match = @("prerequisites installed, but not found yet: vs", "restart the PC"); NotMatch = @() }
    )
    foreach ($c in $cases) {
        foreach ($v in $simVars) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
        foreach ($k in $c.Env.Keys) { Set-Item "Env:$k" $c.Env[$k] }
        $out = (& $ps -NoProfile -ExecutionPolicy Bypass -File "$copy\setup.ps1" -Iso $iso -InstallPrerequisites -Hooks $hooks 2>&1 | Out-String)
        $code = $LASTEXITCODE
        $ok = ($code -eq 1)
        foreach ($m in $c.Match) { if ($out -notmatch $m) { $ok = $false; Write-Host "        missing: $m" } }
        foreach ($m in $c.NotMatch) { if ($out -match $m) { $ok = $false; Write-Host "        unexpected: $m" } }
        Check $ok "$($c.Name) (exit $code)"
        if (-not $ok) { Write-Host ($out -split "`n" | Select-Object -Last 25 | ForEach-Object { "        | $_" }) }
    }
} finally {
    foreach ($v in $simVars) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host "3. an antivirus taking a program built on this PC (build.ps1)"
# (build.ps1's own functions, taken out of it by the PowerShell parser: running it would build)
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Root "build.ps1"), [ref]$null, [ref]$null)
$defs = $ast.FindAll({ param($n) ($n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -in "Get-AntivirusBlockMessage", "Test-AntivirusBlock", "Assert-NotBlocked") -or
                               ($n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$BuiltHere') }, $true)
$Root0 = $Root
$Root = "C:\Games\TFA\transformers-armada-recomp"
foreach ($d in $defs) { Invoke-Expression $d.Extent.Text }
$an = Join-Path ([IO.Path]::GetTempPath()) "ps2_analyzer.exe"; Set-Content $an "x"
$gone = "C:\nowhere\ps2_recomp.exe"
Check (Test-AntivirusBlock $gone 0 "") "a built program gone right after the build"
Check (Test-AntivirusBlock $an 225 "") "exit 225 (ERROR_VIRUS_INFECTED)"
Check (Test-AntivirusBlock $an -1073739514 "") "0xC0000906 (STATUS_VIRUS_INFECTED)"
Check (Test-AntivirusBlock $an -1 "Program 'ps2_analyzer.exe' failed to run: Operation did not complete successfully because the file contains a virus or potentially unwanted software") "PowerShell's 'contains a virus' when starting it"
Check (-not (Test-AntivirusBlock $an 1 "")) "an ordinary failure (exit 1) is not taken for an antivirus"
Check (-not (Test-AntivirusBlock "C:\nowhere\cmake.exe" 0 "")) "only programs built here are judged"
$msg = Get-AntivirusBlockMessage $an
Check ($msg.Contains("Your antivirus blocked ps2_analyzer.exe. It is built on your PC from the open-source code in C:\Games\TFA\transformers-armada-recomp\PS2Recomp\ps2xAnalyzer") -and
       $msg.Contains("Don't turn the antivirus off: add the game folder (C:\Games\TFA\transformers-armada-recomp) as an exclusion") -and $msg.Contains("finished steps are kept")) "the message"
$threw = $null; try { Assert-NotBlocked $gone } catch { $threw = "$_" }
Check ([regex]::Match("$threw", "Your antivirus blocked (\S+?\.exe)\.").Groups[1].Value -eq "ps2_recomp.exe") "setup.ps1 recognises the message (its regex)"
Remove-Item $an
$Root = $Root0

if ($failed) { Write-Host "`n$failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nall checks passed" -ForegroundColor Green
exit 0
