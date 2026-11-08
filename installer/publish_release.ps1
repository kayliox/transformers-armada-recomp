<#
.SYNOPSIS
    Publishes tfa-setup.exe as a GitHub release of its own: one release per setup build, never a
    re-upload over an older one.

.DESCRIPTION
    The README's download button links to releases/latest/download/tfa-setup.exe and its badge counts
    the downloads of every release's assets. Replacing an asset (gh release upload --clobber) deletes
    it and makes a new one, which starts its download count at zero again. So each build is a new
    release:
      - tag setup-v<VERSION> (the VERSION file, e.g. setup-v0.2.2), at the commit it was built from;
      - a second build at the same VERSION gets setup-v<VERSION>-2, then -3 and so on;
      - marked "latest", so /releases/latest/download/tfa-setup.exe is always the newest;
      - a commit that has a release already gets none again (a workflow run repeated).
    The exe holds no game code or data: it runs setup.ps1 on the player's PC with their own disc.

    Needs: gh (GitHub CLI) signed in, or GH_TOKEN (the workflow's token); git.

.EXAMPLE
    .\installer\publish_release.ps1 -Exe installer\target\release\tfa-setup.exe -DryRun
#>
param(
    [Parameter(Mandatory)] [string]$Exe,
    [string]$VersionFile,        # default: VERSION at the repository's top
    [string]$Commit,             # default: HEAD (the workflow passes GITHUB_SHA)
    [string]$Repo,               # owner/name; default: the repository gh finds from this folder
    [switch]$DryRun              # say what would be done, change nothing
)

$ErrorActionPreference = "Stop"
$top = (& git -C $PSScriptRoot rev-parse --show-toplevel).Trim()
if (-not $VersionFile) { $VersionFile = Join-Path $top "VERSION" }
if (-not $Commit) { $Commit = (& git -C $top rev-parse HEAD).Trim() }
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "no such file: $Exe" }
$version = (Get-Content -LiteralPath $VersionFile -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "$VersionFile does not hold a version like 0.2.2: '$version'" }
$repoArgs = if ($Repo) { @("--repo", $Repo) } else { @() }

# The releases there are (tags and the commits they were made at).
$json = & gh release list @repoArgs --limit 200 --json tagName
if ($LASTEXITCODE -ne 0) { throw "gh release list failed" }
$tags = @(($json | ConvertFrom-Json) | ForEach-Object { $_.tagName })
$base = "setup-v$version"
$mine = @($tags | Where-Object { $_ -eq $base -or $_ -match ("^" + [regex]::Escape($base) + '-\d+$') })
foreach ($t in $mine) {
    $at = (& gh release view $t @repoArgs --json targetCommitish --jq .targetCommitish).Trim()
    if ($at -eq $Commit) {
        Write-Host "publish_release: $t was already made from $Commit; nothing to do."
        exit 0
    }
}
$tag = $base
for ($n = 2; $tags -contains $tag; $n++) { $tag = "$base-$n" }

$short = $Commit.Substring(0, [Math]::Min(7, $Commit.Length))
$title = "Setup v$version" + $(if ($tag -ne $base) { " (build $($tag.Substring($base.Length + 1)))" } else { "" })
# The player-facing notes for this version: CHANGELOG.md's "## <version>" section, when there is one.
$whatsNew = ""
$log = Join-Path $top "CHANGELOG.md"
if (Test-Path -LiteralPath $log) {
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = [array]::IndexOf($lines, ($lines | Where-Object { $_ -match ("^## " + [regex]::Escape($version) + '(\s|$)') } | Select-Object -First 1))
    if ($start -ge 0) {
        $end = $start + 1
        while ($end -lt $lines.Count -and $lines[$end] -notmatch '^## ') { $end++ }
        $whatsNew = "## What's new in $version`n" + (($lines[($start + 1)..($end - 1)]) -join "`n").Trim() + "`n`n"
    }
}
$notes = $whatsNew + @"
The setup program for the Transformers (PS2) PC port, v$version (built from $short).

Download tfa-setup.exe, run it, pick your own Transformers (USA, SLUS-20668) disc image and a folder: it installs the tools the build needs, then builds the game on your PC from your disc. The setup contains no game code and no game data.
"@

if ($DryRun) {
    Write-Host "publish_release (dry run): would create $tag ('$title') at $Commit with $Exe, marked latest."
    Write-Host "Existing setup releases for v$($version): $(if ($mine) { $mine -join ', ' } else { 'none' })"
    exit 0
}
$zip = & (Join-Path $PSScriptRoot 'package_recomplauncher.ps1') -Exe $Exe
if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw 'RecompLauncher package was not created' }
& gh release create $tag $Exe $zip @repoArgs --target $Commit --title $title --notes $notes --latest
if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
Write-Host "publish_release: $tag published."
