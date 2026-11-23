# setup_refresh_tests.ps1 - TODO 451: tools\setup_refresh.ps1 (the install folder's tfa-setup.exe brought to the release
# of the version an update moved to), against stand-in releases in a temp folder (TF_SETUP_REFRESH_SOURCE: no network).
#   1. the release picked (setup-v<v>, the newest -<n>, drafts and pre-releases left out), SHA256SUMS.txt read;
#   2. a good file swapped in (the old one kept as tfa-setup.old.exe); one already in place left alone;
#   3. a wrong hash, a wrong size, a file that is not a program, a release without SHA256SUMS.txt, no release for the
#      version: the old one kept every time, nothing left behind;
#   4. a staged copy (its swap failed before) put in place by the next update's start, checked again first.
# The rename of a RUNNING tfa-setup.exe is tested on real PCs (TODO 451's notes), not here.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tests\setup_refresh_tests.ps1   (exit code 0: all passed)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
. (Join-Path $Root "tools\setup_refresh.ps1")
$failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  ok    $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failed++ }
}
function Sha([byte[]]$b) { ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | ForEach-Object { $_.ToString("x2") }) -join "" }
$temp = Join-Path ([IO.Path]::GetTempPath()) ("tfa_refresh_tests_" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory $temp | Out-Null
$oldBytes = [byte[]](@(0x4D, 0x5A) + @(1..200 | ForEach-Object { 7 }))
$newBytes = [byte[]](@(0x4D, 0x5A) + @(1..300 | ForEach-Object { 9 }))
# An install (its checkout in source\, VERSION 0.9.9) with the old setup, and a stand-in release for it.
function New-Case([string]$Name, [byte[]]$Asset, [string]$Sum, [long]$Size = -1, [string]$Tag = "setup-v0.9.9", [switch]$NoSums) {
    $c = Join-Path $temp $Name
    New-Item -ItemType Directory -Force "$c\inst\source", "$c\rel" | Out-Null
    Set-Content "$c\inst\source\VERSION" "0.9.9" -Encoding ASCII
    [IO.File]::WriteAllBytes("$c\inst\tfa-setup.exe", $oldBytes)
    [IO.File]::WriteAllBytes("$c\rel\tfa-setup.exe", $Asset)
    $assets = @(@{ name = "tfa-setup.exe"; size = $(if ($Size -ge 0) { $Size } else { $Asset.Length }) })
    if (-not $NoSums) {
        [IO.File]::WriteAllText("$c\rel\SHA256SUMS.txt", "$Sum  tfa-setup.exe`n0000000000000000000000000000000000000000000000000000000000000000  other.zip`n")
        $assets += @{ name = "SHA256SUMS.txt"; size = 1 }
    }
    @{ tag = $Tag; assets = $assets } | ConvertTo-Json -Depth 4 | Set-Content "$c\rel\release.json" -Encoding UTF8
    return $c
}
function Run-Case([string]$c) {
    $env:TF_SETUP_REFRESH_SOURCE = "$c\rel"
    $out = Update-SetupProgram -Repo "$c\inst\source" -Port "$c\inst\source" 6>&1 | ForEach-Object { "$_" }
    Remove-Item Env:TF_SETUP_REFRESH_SOURCE
    return ($out -join " | ")
}
function Bytes([string]$p) { if (Test-Path -LiteralPath $p) { Sha ([IO.File]::ReadAllBytes($p)) } else { "" } }
function Leftovers([string]$c) { @(Get-ChildItem "$c\inst" -File | Where-Object { $_.Name -ne "tfa-setup.exe" -and $_.Name -ne "tfa-setup.old.exe" }).Count }
try {
    Write-Host "1. the release and its checksums"
    $rel = @([pscustomobject]@{ tag_name = "setup-v0.9.9"; draft = $false; prerelease = $false },
             [pscustomobject]@{ tag_name = "setup-v0.9.9-3"; draft = $false; prerelease = $false },
             [pscustomobject]@{ tag_name = "setup-v0.9.9-4"; draft = $true; prerelease = $false },
             [pscustomobject]@{ tag_name = "setup-v0.9.10"; draft = $false; prerelease = $false },
             [pscustomobject]@{ tag_name = "setup-v0.9.99"; draft = $false; prerelease = $false })
    Check ((Select-SetupRelease $rel "0.9.9").tag_name -eq "setup-v0.9.9-3") "the newest build of the version (a draft left out, 0.9.10 / 0.9.99 not taken for 0.9.9)"
    Check ($null -eq (Select-SetupRelease $rel "0.9.8")) "no release for another version"
    Check ((Get-SumFor "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789 *tfa-setup.exe`n" "tfa-setup.exe") -eq "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789") "SHA256SUMS.txt read (binary-mode star, upper case)"
    Check ((Get-GitHubRepoName "https://github.com/kayliox/transformers-armada-recomp.git") -eq "kayliox/transformers-armada-recomp") "a GitHub address read"
    Check ($null -eq (Get-GitHubRepoName "D:\Games\origin.git")) "a local origin is not GitHub"

    Write-Host "2. a good file"
    $c = New-Case "good" $newBytes (Sha $newBytes)
    $said = Run-Case $c
    Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $newBytes)) "swapped in ($said)"
    Check ((Bytes "$c\inst\tfa-setup.old.exe") -eq (Sha $oldBytes)) "the old one kept as tfa-setup.old.exe"
    Check ((Leftovers $c) -eq 0) "nothing else left in the install folder"
    $said = Run-Case $c
    Check ($said -eq "" -and (Bytes "$c\inst\tfa-setup.exe") -eq (Sha $newBytes)) "already that release's: nothing done, nothing said"

    Write-Host "3. files that are not used"
    foreach ($bad in @(@{ n = "hash"; a = $newBytes; s = ("ab" * 32); z = -1 }, @{ n = "size"; a = $newBytes; s = (Sha $newBytes); z = 999 },
                       @{ n = "notexe"; a = [byte[]](1..50); s = (Sha ([byte[]](1..50))); z = -1 })) {
        $c = New-Case "bad_$($bad.n)" $bad.a $bad.s $bad.z
        $said = Run-Case $c
        Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $oldBytes) -and (Leftovers $c) -eq 0 -and -not (Test-Path "$c\inst\tfa-setup.old.exe") -and $said -like "*kept*") "wrong $($bad.n): the old one kept ($said)"
    }
    $c = New-Case "nosums" $newBytes "" -NoSums
    $said = Run-Case $c
    Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $oldBytes) -and $said -like "*no checksums*") "a release without SHA256SUMS.txt: kept ($said)"
    $c = New-Case "noversion" $newBytes (Sha $newBytes) -Tag "setup-v0.9.8"
    $said = Run-Case $c
    Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $oldBytes) -and $said -like "*no release*") "no release for the version: kept ($said)"
    $c = New-Case "off" $newBytes (Sha $newBytes)
    $env:TF_SETUP_REFRESH = "0"; $said = Run-Case $c; Remove-Item Env:TF_SETUP_REFRESH
    Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $oldBytes)) "TF_SETUP_REFRESH=0: nothing done"

    Write-Host "4. a staged copy"
    $c = New-Case "staged" $newBytes (Sha $newBytes)
    [IO.File]::WriteAllBytes("$c\inst\tfa-setup.new.exe", $newBytes)
    Set-Content "$c\inst\tfa-setup.new.sha256" "$(Sha $newBytes) setup-v0.9.9" -Encoding ASCII
    [IO.File]::WriteAllBytes("$c\inst\tfa-setup.old.exe", [byte[]](1, 2, 3))
    $said = (Complete-StagedSetupProgram -Repo "$c\inst\source" 6>&1 | ForEach-Object { "$_" }) -join " | "
    Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $newBytes) -and (Bytes "$c\inst\tfa-setup.old.exe") -eq (Sha $oldBytes) -and (Leftovers $c) -eq 0) "put in place at the next start, the older .old replaced ($said)"
    $c = New-Case "staged_bad" $newBytes (Sha $newBytes)
    [IO.File]::WriteAllBytes("$c\inst\tfa-setup.new.exe", $newBytes)
    Set-Content "$c\inst\tfa-setup.new.sha256" "$(Sha $oldBytes) setup-v0.9.9" -Encoding ASCII
    $said = (Complete-StagedSetupProgram -Repo "$c\inst\source" 6>&1 | ForEach-Object { "$_" }) -join " | "
    Check ((Bytes "$c\inst\tfa-setup.exe") -eq (Sha $oldBytes) -and (Leftovers $c) -eq 0) "a staged copy that no longer matches its checksum: deleted, not used ($said)"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { Write-Host "$failed FAILED" -ForegroundColor Red; exit 1 }
Write-Host "all passed"
exit 0
