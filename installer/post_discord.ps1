# Posts a new version's release notes to the Discord server's updates channel through a channel webhook.
# Input: -Tag (the release the workflow just published, setup-v<VERSION>); env DISCORD_WEBHOOK (a repository
# secret: Discord channel > Edit Channel > Integrations > Webhooks > Copy Webhook URL). Without the secret it does
# nothing. Rebuild releases (setup-v<VERSION>-2 ...) are skipped, so each version is announced once.
# The text is CHANGELOG.md's "## <VERSION>" section (the same plain-language notes as the GitHub release).
param([Parameter(Mandatory = $true)][string]$Tag, [switch]$DryRun)
# -DryRun prints the message instead of posting it (works without the secret).
$ErrorActionPreference = 'Stop'

if (-not $env:DISCORD_WEBHOOK -and -not $DryRun) { Write-Host 'post_discord: no DISCORD_WEBHOOK secret, nothing posted.'; exit 0 }
if ($Tag -notmatch '^setup-v(\d+\.\d+\.\d+)$') { Write-Host "post_discord: $Tag is a rebuild or not a version release, not announced."; exit 0 }
$version = $Matches[1]

$top = Split-Path -Parent $PSScriptRoot
$notes = ''
$log = Join-Path $top 'CHANGELOG.md'
if (Test-Path -LiteralPath $log) {
    $lines = Get-Content -LiteralPath $log -Encoding UTF8
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match ('^## ' + [regex]::Escape($version) + '(\s|$)')) { $start = $i; break } }
    if ($start -ge 0) {
        $end = $start + 1
        while ($end -lt $lines.Count -and $lines[$end] -notmatch '^## ') { $end++ }
        if ($end -gt $start + 1) { $notes = (($lines[($start + 1)..($end - 1)]) -join "`n").Trim() }
    }
}
if (-not $notes) { $notes = 'Fixes and improvements.' }

$repo = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { 'kayliox/transformers-armada-recomp' }
$url = "https://github.com/$repo/releases/tag/$Tag"
$footer = "`n`n**How to update:** in the game, F1 > About > Update now. New players: download tfa-setup.exe from the release page."
# Discord allows 4096 characters in an embed description; cut long notes at a line and point to the full page.
$max = 4096 - $footer.Length - 80
if ($notes.Length -gt $max) {
    $cut = $notes.LastIndexOf("`n", $max)
    if ($cut -lt 1) { $cut = $max }
    $notes = $notes.Substring(0, $cut).TrimEnd() + "`n`n...and more on the release page."
}

$body = @{
    username = 'Transformers Armada updates'
    embeds   = @(@{
        title       = "Version $version is out"
        url         = $url
        description = $notes + $footer
        color       = 3447003
    })
    allowed_mentions = @{ parse = @() }
} | ConvertTo-Json -Depth 6

if ($DryRun) { Write-Host $body; exit 0 }
Invoke-RestMethod -Method Post -Uri $env:DISCORD_WEBHOOK -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body)) | Out-Null
Write-Host "post_discord: announced $version."
