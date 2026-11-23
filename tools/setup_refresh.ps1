# (TODO 451) The setup program in the install folder (tfa-setup.exe) brought up to date with the game.
#
# A player's install keeps the tfa-setup.exe of their first setup: the game's "Update now" starts it, so a newer
# setup's window (the small updater window in the F1 menu's look) would never reach them. After an update, update.ps1
# calls Update-SetupProgram: it looks for the GitHub release of the setup made for the version the copy is at now
# (tag setup-v<VERSION>, or setup-v<VERSION>-<n> for a later build of it, the newest), and when its tfa-setup.exe is
# not the one in the install folder already, downloads it and checks it against the release (the size GitHub records
# for the file, and the SHA-256 in the release's SHA256SUMS.txt, which publish_release.ps1 uploads). Only then is it
# swapped in: Windows lets a running program's file be RENAMED (not overwritten or deleted), so the running
# tfa-setup.exe becomes tfa-setup.old.exe and the new one takes its name; the old program goes on working from its
# renamed file, and the next start is the new one. If the move fails the old one gets its name back. If the rename
# itself fails, the new file stays staged (tfa-setup.new.exe, with its hash in tfa-setup.new.sha256) and the next
# update.ps1 run swaps it in at its start (Complete-StagedSetupProgram), after checking it again.
#
# Nothing here can fail an update: every problem keeps the old setup program and says why in one line (the update's
# log). Only the setup program's own release asset is downloaded; nothing else.
#
# Switches: TF_SETUP_REFRESH=0 (off); TF_SETUP_REFRESH_SOURCE=<folder> (tests: a stand-in release, no network: its
# release.json {"tag": ..., "assets": [{"name": ..., "size": ...}]}, the files beside it, SHA256SUMS.txt among them).

function Write-SetupRefresh([string]$Text) {
    Write-Host "Setup program: $Text"
    if (Get-Command Write-StepLog -ErrorAction SilentlyContinue) { Write-StepLog "update: setup program: $Text" }
}

# owner/name of a github.com repository URL (https or ssh form), else $null.
function Get-GitHubRepoName([string]$Url) {
    $m = [regex]::Match("$Url".Trim(), 'github\.com[/:]([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(\.git)?/?$')
    if ($m.Success) { return "$($m.Groups[1].Value)/$($m.Groups[2].Value)" }
    return $null
}

# The newest setup release for a version: tag setup-v<v> (build 1) or setup-v<v>-<n>; drafts and pre-releases left out.
function Select-SetupRelease($Releases, [string]$Version) {
    $pattern = '^setup-v' + [regex]::Escape($Version) + '(-(\d+))?$'
    $best = $null; $bestN = 0
    foreach ($r in @($Releases)) {
        if ($r.draft -or $r.prerelease) { continue }
        $m = [regex]::Match("$($r.tag_name)", $pattern)
        if (-not $m.Success) { continue }
        $n = if ($m.Groups[2].Success) { [int]$m.Groups[2].Value } else { 1 }
        if ($n -gt $bestN) { $best = $r; $bestN = $n }
    }
    return $best
}

# The hash SHA256SUMS.txt gives a file ("<hex>  <name>" lines; "*<name>" for binary mode too), lower case, else $null.
function Get-SumFor([string]$Sums, [string]$Name) {
    foreach ($line in ("$Sums" -split "`r?`n")) {
        $m = [regex]::Match($line.Trim(), '^([0-9A-Fa-f]{64})\s+\*?(.+)$')
        if ($m.Success -and $m.Groups[2].Value.Trim() -eq $Name) { return $m.Groups[1].Value.ToLowerInvariant() }
    }
    return $null
}

function Get-FileSha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

# A Windows program file: it starts with "MZ".
function Test-ProgramFile([string]$Path) {
    try {
        $fs = [IO.File]::OpenRead($Path)
        try { $b = New-Object byte[] 2; return ($fs.Read($b, 0, 2) -eq 2 -and $b[0] -eq 0x4D -and $b[1] -eq 0x5A) } finally { $fs.Dispose() }
    } catch { return $false }
}

# The running (or not) tfa-setup.exe renamed out of the way and the staged one moved in. $true when swapped.
function Invoke-SetupSwap([string]$Root, [string]$Expected) {
    $cur = Join-Path $Root "tfa-setup.exe"
    $new = Join-Path $Root "tfa-setup.new.exe"
    if ((Get-FileSha256 $new) -ne $Expected) { throw "the downloaded file changed on the disk before it was put in place" }
    # (an older copy kept from the last time: deleted when nothing runs from it, else a numbered name beside it)
    $old = Join-Path $Root "tfa-setup.old.exe"
    if (Test-Path -LiteralPath $old) {
        try { Remove-Item -LiteralPath $old -Force -ErrorAction Stop }
        catch {
            $n = 2
            while (Test-Path -LiteralPath (Join-Path $Root "tfa-setup.old$n.exe")) { $n++ }
            $old = Join-Path $Root "tfa-setup.old$n.exe"
        }
    }
    if (Test-Path -LiteralPath $cur) { Move-Item -LiteralPath $cur -Destination $old -ErrorAction Stop }
    try { Move-Item -LiteralPath $new -Destination $cur -ErrorAction Stop }
    catch {
        if (Test-Path -LiteralPath $old) { Move-Item -LiteralPath $old -Destination $cur -ErrorAction SilentlyContinue }
        throw
    }
    Remove-Item -LiteralPath (Join-Path $Root "tfa-setup.new.sha256") -Force -ErrorAction SilentlyContinue
    # (the numbered copies of earlier times that nothing runs from any more)
    Get-ChildItem -LiteralPath $Root -Filter "tfa-setup.old*.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -ne $old } | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue }
    return $true
}

# At the start of an update: a setup program staged last time (its swap failed then) put in place now, checked again.
function Complete-StagedSetupProgram([string]$Repo) {
    try {
        $root = Split-Path $Repo -Parent
        $new = Join-Path $root "tfa-setup.new.exe"
        $mark = Join-Path $root "tfa-setup.new.sha256"
        if (-not (Test-Path -LiteralPath $new)) { return }
        $want = if (Test-Path -LiteralPath $mark) { ("" + (Get-Content -LiteralPath $mark -TotalCount 1)).Trim().Split(' ')[0].ToLowerInvariant() } else { "" }
        if ($want -notmatch '^[0-9a-f]{64}$') {
            Remove-Item -LiteralPath $new, $mark -Force -ErrorAction SilentlyContinue
            Write-SetupRefresh "a staged copy without its checksum was deleted"
            return
        }
        if ((Get-FileSha256 $new) -ne $want) {
            Remove-Item -LiteralPath $new, $mark -Force -ErrorAction SilentlyContinue
            Write-SetupRefresh "a copy downloaded last time no longer matched its checksum and was deleted; the old one is used"
            return
        }
        if (Invoke-SetupSwap $root $want) { Write-SetupRefresh "the copy downloaded last time is in place now (the old one kept as tfa-setup.old.exe)" }
    } catch {
        Write-SetupRefresh "the copy downloaded last time is still waiting ($($_.Exception.Message)); the old one is used"
    }
}

function Update-SetupProgram([string]$Repo, [string]$Port) {
    $ProgressPreference = "SilentlyContinue"   # (Windows PowerShell's progress bar slows a download many times over)
    try {
        if ($env:TF_SETUP_REFRESH -eq "0") { return }
        $root = Split-Path $Repo -Parent
        $cur = Join-Path $root "tfa-setup.exe"
        if (-not (Test-Path -LiteralPath $cur)) { return }   # (a copy set up without the setup program: nothing to keep up to date)
        $version = try { ("" + (Get-Content -LiteralPath (Join-Path $Port "VERSION") -TotalCount 1)).Trim() } catch { "" }
        if ($version -notmatch '^\d+\.\d+\.\d+$') { return }
        $source = $env:TF_SETUP_REFRESH_SOURCE
        $headers = @{ "User-Agent" = "tfa-update"; "Accept" = "application/vnd.github+json" }
        if ($source) {
            $j = Get-Content -LiteralPath (Join-Path $source "release.json") -Raw | ConvertFrom-Json
            $assets = @($j.assets | ForEach-Object { [pscustomobject]@{ name = $_.name; size = [long]$_.size; browser_download_url = (Join-Path $source $_.name) } })
            $releases = @([pscustomobject]@{ tag_name = $j.tag; draft = $false; prerelease = $false; assets = $assets })
        } else {
            $url = (& git -C $Repo remote get-url origin 2>$null | Select-Object -First 1)
            $name = Get-GitHubRepoName $url
            if (-not $name) { return }   # (not a GitHub copy: no releases to look in)
            try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
            $releases = Invoke-RestMethod -Uri "https://api.github.com/repos/$name/releases?per_page=40" -Headers $headers -TimeoutSec 30 -UseBasicParsing
        }
        $release = Select-SetupRelease $releases $version
        if (-not $release) { Write-SetupRefresh "no release of it for $version yet: the one you have is kept"; return }
        $exe = @($release.assets | Where-Object { $_.name -eq "tfa-setup.exe" }) | Select-Object -First 1
        $sumsAsset = @($release.assets | Where-Object { $_.name -eq "SHA256SUMS.txt" }) | Select-Object -First 1
        if (-not $exe -or -not $sumsAsset) { Write-SetupRefresh "the release $($release.tag_name) has no checksums to check it with: the one you have is kept"; return }
        $size = [long]$exe.size
        if ($size -le 0 -or $size -gt 64MB) { Write-SetupRefresh "the release's file has an unexpected size ($size bytes): the one you have is kept"; return }
        $tmpSums = Join-Path $root "tfa-setup.sums.tmp"
        $tmp = Join-Path $root "tfa-setup.download.tmp"
        try {
            if ($source) { Copy-Item -LiteralPath $sumsAsset.browser_download_url -Destination $tmpSums -Force }
            else { Invoke-WebRequest -Uri $sumsAsset.browser_download_url -Headers $headers -OutFile $tmpSums -TimeoutSec 60 -UseBasicParsing }
            $want = Get-SumFor (Get-Content -LiteralPath $tmpSums -Raw) "tfa-setup.exe"
        } finally { Remove-Item -LiteralPath $tmpSums -Force -ErrorAction SilentlyContinue }
        if (-not $want) { Write-SetupRefresh "the release's checksums do not name tfa-setup.exe: the one you have is kept"; return }
        if ((Get-FileSha256 $cur) -eq $want) {
            Remove-Item -LiteralPath (Join-Path $root "tfa-setup.new.exe"), (Join-Path $root "tfa-setup.new.sha256") -Force -ErrorAction SilentlyContinue
            return   # (already that release's)
        }
        try {
            if ($source) { Copy-Item -LiteralPath $exe.browser_download_url -Destination $tmp -Force }
            else { Invoke-WebRequest -Uri $exe.browser_download_url -Headers $headers -OutFile $tmp -TimeoutSec 120 -UseBasicParsing }
            $got = (Get-Item -LiteralPath $tmp).Length
            if ($got -ne $size) { throw "the download is $got bytes, the release's file $size" }
            if ((Get-FileSha256 $tmp) -ne $want) { throw "the download's SHA-256 is not the one the release publishes" }
            if (-not (Test-ProgramFile $tmp)) { throw "the download is not a Windows program" }
            Move-Item -LiteralPath $tmp -Destination (Join-Path $root "tfa-setup.new.exe") -Force -ErrorAction Stop
            Set-Content -LiteralPath (Join-Path $root "tfa-setup.new.sha256") -Value "$want $($release.tag_name)" -Encoding ASCII
        } catch {
            Remove-Item -LiteralPath $tmp, (Join-Path $root "tfa-setup.new.exe"), (Join-Path $root "tfa-setup.new.sha256") -Force -ErrorAction SilentlyContinue
            throw
        }
        try {
            if (Invoke-SetupSwap $root $want) { Write-SetupRefresh "updated to the one of $($release.tag_name) (the old one kept as tfa-setup.old.exe)" }
        } catch {
            Write-SetupRefresh "the new one ($($release.tag_name)) is downloaded and checked, and goes in place at the next update ($($_.Exception.Message))"
        }
    } catch {
        Write-SetupRefresh "not updated ($($_.Exception.Message)); the one you have is kept"
    }
}
