# clear-cover.ps1 - strip embedded cover art (APIC) from MP3 files.
#
# Plan:
#   1. ffprobe detects whether a file carries a cover (video stream).
#      Files without cover art are left 100% untouched.
#   2. Files with cover art are remuxed with ffmpeg -c copy (lossless
#      audio stream copy): cover stream dropped, all other ID3 tags kept
#      (including ReplayGain TXXX), output forced to ID3v2.3.
#   3. Temp file in the same directory, then atomic replace.
#   4. Afterwards re-run rsgain-scan.ps1 as a safety net (full rescan by
#      default; -Incremental keeps the tags that survived the remux).
#
# Usage: powershell -ExecutionPolicy Bypass -File clear-cover.ps1 D:\Music
#
# Double-clicked from Explorer, the script stops at a "Press Enter to close"
# prompt when it finishes, so the summary does not vanish with the window.
# -Pause / -NoPause override that detection.

param(
    [Parameter(Position = 0)][string]$Root,
    [switch]$Pause,
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"

# ---- 结束时的窗口停留 ----------------------------------------------------
# 双击运行（资源管理器里双击 .bat / .ps1）时窗口会随脚本结束关闭。
# 沿父进程链往上找，第一个"非 shell 类祖先"若是 explorer.exe，说明是
# 用户在资源管理器里双击启动的 —— 此时停在"按 Enter 关闭"上；
# 从终端 / 自动化调用时不阻塞。
$script:ShellHosts = @('powershell.exe', 'pwsh.exe', 'cmd.exe', 'conhost.exe',
                       'bash.exe', 'sh.exe', 'zsh.exe')

function Test-DoubleClicked {
    try {
        $all = @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop)
        $map = @{}
        foreach ($p in $all) { $map[[int]$p.ProcessId] = $p }

        $cur = $PID
        for ($i = 0; $i -lt 6; $i++) {
            $proc = $map[$cur]
            if (-not $proc) { return $false }

            $parentId = [int]$proc.ParentProcessId
            if ($parentId -le 4) { return $false }
            $parent = $map[$parentId]
            if (-not $parent) { return $false }

            $name = ([string]$parent.Name).ToLowerInvariant()
            if ($name -eq 'explorer.exe') { return $true }
            if ($script:ShellHosts -notcontains $name) { return $false }

            $cur = $parentId
        }
    } catch {
        return $false
    }
    return $false
}

function Test-ShouldPause {
    if ($Pause) { return $true }
    if ($NoPause) { return $false }
    return (Test-DoubleClicked)
}

function Wait-AtEnd {
    Write-Host ""
    Write-Host "All done. Press Enter to close this window." -ForegroundColor Cyan
    try { [void](Read-Host) } catch { }
}

$root = $Root
if (-not $root) {
    $root = Read-Host "Enter music folder path"
}
if (-not (Test-Path -LiteralPath $root)) {
    Write-Host "[ERROR] folder not found: $root" -ForegroundColor Red
    if (Test-ShouldPause) { Wait-AtEnd }
    exit 1
}

$ffmpeg  = "D:\Software\ffmpeg-6.1.1-essentials_build\bin\ffmpeg.exe"
$ffprobe = "D:\Software\ffmpeg-6.1.1-essentials_build\bin\ffprobe.exe"
if (-not (Test-Path -LiteralPath $ffmpeg)) {
    $c = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($c) { $ffmpeg = $c.Source } else { Write-Host "[ERROR] ffmpeg not found" -ForegroundColor Red; exit 1 }
}
if (-not (Test-Path -LiteralPath $ffprobe)) {
    $c = Get-Command ffprobe -ErrorAction SilentlyContinue
    if ($c) { $ffprobe = $c.Source } else { Write-Host "[ERROR] ffprobe not found" -ForegroundColor Red; exit 1 }
}

$files = Get-ChildItem -LiteralPath $root -Filter *.mp3 -Recurse
Write-Host ("Found {0} MP3 files under {1}" -f $files.Count, $root)

$processed = 0
$skipped   = 0
$failed    = 0

foreach ($f in $files) {
    # 1. detect cover art (any video stream)
    $art = & $ffprobe -v error -select_streams v -show_entries stream=codec_type -of csv=p=0 $f.FullName 2>$null
    if (-not $art) {
        $skipped++
        continue
    }

    # 2. remux: audio stream copy, drop cover, keep tags, ID3v2.3
    $tmp = $f.FullName + ".noart.tmp.mp3"
    & $ffmpeg -y -hide_banner -loglevel error -i $f.FullName `
        -map 0:a -c copy -map_metadata 0 -id3v2_version 3 -write_id3v1 0 $tmp
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $tmp)) {
        Write-Host ("[FAIL] {0}" -f $f.Name) -ForegroundColor Red
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
        $failed++
        continue
    }

    # 3. atomic replace
    Move-Item -LiteralPath $tmp -Destination $f.FullName -Force
    $processed++
    Write-Host ("[{0}] stripped cover: {1}" -f $processed, $f.Name)
}

Write-Host ""
Write-Host ("Done. stripped={0} untouched={1} failed={2}" -f $processed, $skipped, $failed)
Write-Host "Tip: re-run rsgain-scan.ps1 afterwards (full rescan by default)."
if (Test-ShouldPause) { Wait-AtEnd }