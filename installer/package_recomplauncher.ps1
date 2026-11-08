<#
.SYNOPSIS
    Packages the setup and its RecompLauncher manifest, without any game data.
#>
param(
    [Parameter(Mandatory)][string]$Exe,
    [string]$Output
)
$ErrorActionPreference = 'Stop'
$top = Split-Path $PSScriptRoot -Parent
$manifest = Join-Path $top 'recomplauncher.json'
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "No such setup: $Exe" }
if ((Split-Path $Exe -Leaf) -ne 'tfa-setup.exe') { throw 'The setup must be named tfa-setup.exe' }
$profile = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if ($profile.format -ne 1 -or $profile.prepare.program[0] -ne 'tfa-setup.exe') { throw 'Invalid RecompLauncher manifest' }
$version = (Get-Content -LiteralPath (Join-Path $top 'VERSION') -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid VERSION' }
if (-not $Output) {
    $Output = Join-Path (Split-Path $Exe -Parent) "TransformersArmada-v$version-Windows-x64.zip"
}
# Two top-level files only. Never package a source checkout, extracted disc, save or generated build.
Compress-Archive -LiteralPath @($Exe, $manifest) -DestinationPath $Output -Force
Write-Output $Output
