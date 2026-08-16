# clear-cover.ps1 - strip embedded cover art (APIC) from MP3 files.
#
# Plan:
#   1. ffprobe detects whether a file carries a cover (video stream).
#      Files without cover art are left 100% untouched.
#   2. Files with cover art are remuxed with ffmpeg -c copy (lossless
#      audio stream copy): cover stream dropped, all other ID3 tags kept
#      (including ReplayGain TXXX), output forced to ID3v2.3.
#   3. Temp file in the same directory, then atomic replace.
#   4. Afterwards re-run rsgain-scan.bat as a safety net (-S skips files
#      that still have ReplayGain tags).
#
# Usage: powershell -ExecutionPolicy Bypass -File clear-cover.ps1 D:\Music

$ErrorActionPreference = "Stop"

$root = $args[0]
if (-not $root) {
    $root = Read-Host "Enter music folder path"
}
if (-not (Test-Path -LiteralPath $root)) {
    Write-Host "[ERROR] folder not found: $root" -ForegroundColor Red
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
Write-Host "Tip: re-run rsgain-scan.bat afterwards (-S skips still-tagged files)."