<#
.SYNOPSIS
    (TODO 439) Frees disk space after the game is built: deletes the build's in-between files, which playing never needs.

.DESCRIPTION
    The build leaves three kinds of files in the game's folder:
      the game itself     out\build\transformers\<config>\ (TransformersRecomp.exe, its DLLs and its .pdb): ALWAYS KEPT.
      temporary files     work\staging: the recompiler's raw output, copied into work\generated and written again
                          by every translation. Nothing reads it afterwards: deleting it changes nothing.
      in-between files    the rest of out\build (the compiler's object files), out\tools (the recompiler's own build)
                          and work\ (the translated C++ code). Updates use them to rebuild only what changed.
                          Without them the NEXT update rebuilds the whole game once (about as long as the first
                          build) and leaves them behind again.

    -Level temp  deletes the temporary files only (no cost).
    -Level all   deletes the temporary files and the in-between files (the next update is a full rebuild).
    -Report      deletes nothing; one line per kind: "<kind>|<bytes>|<files>" (kinds: game, temp, build), for the
                 setup program and setup.ps1.
    Never while the game runs from this folder. After -Level all it leaves out\build-cleaned.txt: update_forecast.ps1
    reads it (until the next build) to say up front that the next update rebuilds the whole game.

    Deps: Windows PowerShell 5.1 or newer; cmd.exe's rd (for long paths). Linux: tools/clean_build.sh.

.EXAMPLE
    .\tools\clean_build.ps1 -Report
.EXAMPLE
    .\tools\clean_build.ps1 -Level all
#>
param(
    [ValidateSet("", "temp", "all")][string]$Level = "",
    [switch]$Report,
    [string]$Port = (Split-Path $PSScriptRoot -Parent)   # the game's folder (out\ and work\ are in it)
)
$ErrorActionPreference = "Stop"
$Port = (Resolve-Path -LiteralPath $Port).Path
$out = Join-Path $Port "out"
$build = Join-Path $out "build"
$tools = Join-Path $out "tools"
$work = Join-Path $Port "work"

# The game's own folder: the one with TransformersRecomp.exe (RelWithDebInfo unless the build was made otherwise).
function Get-GameFolder {
    $t = Join-Path $build "transformers"
    if (-not (Test-Path -LiteralPath $t)) { return $null }
    $exe = Get-ChildItem -LiteralPath $t -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "TransformersRecomp.exe") } | Select-Object -First 1
    if ($exe) { return $exe.FullName }
    return $null
}

# The paths of each kind (folders and files, whole). "build" never holds the game's folder or anything above it.
function Get-Kinds {
    $game = Get-GameFolder
    $kinds = [ordered]@{ game = @(); temp = @(); build = @() }
    if ($game) { $kinds.game = @($game) }
    $staging = Join-Path $work "staging"
    if (Test-Path -LiteralPath $staging) { $kinds.temp = @($staging) }
    $b = @()
    if (Test-Path -LiteralPath $tools) { $b += $tools }
    if (Test-Path -LiteralPath $work) {
        $b += @(Get-ChildItem -LiteralPath $work -Force | Where-Object { $_.FullName -ne $staging } | ForEach-Object FullName)
    }
    if (Test-Path -LiteralPath $build) {
        if ($game) {
            # out\build without the game's folder: everything beside the path down to it
            $keepParent = Split-Path $game -Parent   # out\build\transformers
            $b += @(Get-ChildItem -LiteralPath $build -Force | Where-Object { $_.FullName -ne $keepParent } | ForEach-Object FullName)
            $b += @(Get-ChildItem -LiteralPath $keepParent -Force | Where-Object { $_.FullName -ne $game } | ForEach-Object FullName)
        } else {
            $b += $build   # (no game built: all of it is in-between files)
        }
    }
    $kinds.build = $b
    return $kinds
}

function Get-Size([string[]]$Paths) {
    $bytes = 0L; $files = 0
    foreach ($p in $Paths) {
        if (Test-Path -LiteralPath $p -PathType Leaf) { $bytes += (Get-Item -LiteralPath $p -Force).Length; $files++; continue }
        foreach ($f in [IO.Directory]::EnumerateFiles($p, "*", [IO.SearchOption]::AllDirectories)) {
            try { $bytes += ([IO.FileInfo]::new($f)).Length; $files++ } catch {}
        }
    }
    return @($bytes, $files)
}

function Format-GB([long]$Bytes) { "{0:N1} GB" -f ($Bytes / 1GB) }

$kinds = Get-Kinds
if ($Report -or -not $Level) {
    foreach ($k in $kinds.Keys) { $s = Get-Size $kinds[$k]; Write-Output ("{0}|{1}|{2}" -f $k, $s[0], $s[1]) }
    exit 0
}

# Not while the game runs from here (it holds its own folder only, but an update could be starting from it).
$running = Get-Process TransformersRecomp -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Port, [StringComparison]::OrdinalIgnoreCase) }
if ($running) { Write-Host "clean_build: the game is running from this folder: close it first" -ForegroundColor Red; exit 2 }

$doomed = @($kinds.temp)
if ($Level -eq "all") { $doomed += $kinds.build }
$size = Get-Size $doomed
$failed = @()
foreach ($p in $doomed) {
    if (-not (Test-Path -LiteralPath $p)) { continue }
    if (Test-Path -LiteralPath $p -PathType Leaf) {
        try { Remove-Item -LiteralPath $p -Force } catch { $failed += $p }
        continue
    }
    # (rd with the \\?\ form: the build's deepest paths are longer than Windows PowerShell's 260-character limit)
    & cmd.exe /d /c "rd /s /q `"\\?\$p`"" 2>$null | Out-Null
    if (Test-Path -LiteralPath $p) { $failed += $p }
}
if ($Level -eq "all") {
    New-Item -ItemType Directory -Force $out | Out-Null
    Set-Content -LiteralPath (Join-Path $out "build-cleaned.txt") -Value ("in-between files deleted {0}; the next build rebuilds the whole game" -f (Get-Date -Format s)) -Encoding ASCII
}
$what = if ($Level -eq "all") { "the build's temporary and in-between files" } else { "the build's temporary files" }
Write-Host ("Deleted {0}: {1} freed ({2} files)." -f $what, (Format-GB $size[0]), $size[1]) -ForegroundColor Green
if ($Level -eq "all") { Write-Host "The game plays as before. The next update rebuilds the whole game once (about as long as the first build)." }
if ($failed.Count) {
    Write-Host ("Could not delete (in use?): " + ($failed -join "; ")) -ForegroundColor Yellow
    exit 1
}
exit 0
