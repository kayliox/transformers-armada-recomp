# The libraries the build uses, fetched once as pinned, hash-checked archives (tools/fetch_deps.py, tools/deps.json),
# as CMake arguments: -DFETCHCONTENT_SOURCE_DIR_<NAME>=<folder>. CMake then needs no network and no git for them.
# A library the build folder already holds (fetched by CMake in an earlier build) is kept as it is, and one that cannot
# be fetched is left to CMake's own fetch: each gets an empty value, so a folder set before never sticks.
# Dot-sourced by build.ps1. TF_DEPS=0: CMake fetches everything itself, as before.
function Get-DepDefines([string]$Root, [string]$Set, [string]$BuildDir) {
    $manifest = Join-Path $Root "tools\deps.json"
    if (-not (Test-Path -LiteralPath $manifest)) { return @() }
    $names = @()
    try { $names = @((Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json).deps | Where-Object { $_.set -eq $Set } | ForEach-Object { "$($_.name)".ToUpperInvariant() }) } catch { return @() }
    $found = @{}
    $ErrorActionPreference = "Continue"
    try {
        # (TODO 444: the build's own Python, build.ps1's TF_PYTHON, not whatever "python" PATH finds first)
        $py = if ($env:TF_PYTHON) { $env:TF_PYTHON } else { "python" }
        $lines = & $py (Join-Path $Root "tools\fetch_deps.py") --set $Set --build $BuildDir --deps (Join-Path $Root "out\deps") 2>&1 | ForEach-Object { "$_" }
        foreach ($l in $lines) {
            if ($l -match '^([A-Z0-9_]+)=(.*)$') { $found[$Matches[1]] = $Matches[2] }
            elseif ($l) { Write-Host "  $l"; Write-StepLog "build: $l" }
        }
    } catch { Write-Host "  deps: $($_.Exception.Message): CMake fetches the libraries itself" }
    $ErrorActionPreference = "Stop"
    @($names | ForEach-Object { "-DFETCHCONTENT_SOURCE_DIR_$_=$(if ($found.ContainsKey($_)) { $found[$_] })" })
}
