# (TODO 310) The steps' own record, for a PC that switches itself off during the setup or an update (a player's PC
# reset with 8 BIOS beeps near the end of the last bar, and nobody could say which step it was).
#
# setup.ps1 and update.ps1 (or the setup window that runs them) set TF_STEP_LOG to a file; then every step and
# sub-stage of setup.ps1, update.ps1, build.ps1 and make_hd_textures.ps1 adds one line to it with the clock time:
#
#     2026-11-13 14:03:12  build: Building TransformersRecomp (RelWithDebInfo) - about 329 file(s) to compile, about 9 min
#     2026-11-13 14:05:40  build:     compiling the game: 120 of about 329 files, about 6 min left
#
# Each line is written straight through to the disk (no write cache), so it is there even when the PC goes off a
# moment later; the console and setup.log are not (Windows keeps their last lines in memory for a while). The setup
# window shows the last line when a setup or an update did not finish. Without TF_STEP_LOG nothing is written.

function Write-StepLog([string]$Text) {
    $path = $env:TF_STEP_LOG
    if (-not $path) { return }
    try {
        $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::ReadWrite, 4096, [System.IO.FileOptions]::WriteThrough)
        try {
            $bytes = [System.Text.Encoding]::UTF8.GetBytes("$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $Text`r`n")
            $fs.Write($bytes, 0, $bytes.Length)
            $fs.Flush($true)
        } finally { $fs.Dispose() }
    } catch {}
}
