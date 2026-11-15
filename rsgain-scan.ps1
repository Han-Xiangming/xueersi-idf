#Requires -Version 5.1
<#
.SYNOPSIS
    扫描 SD 卡曲库并写入 ReplayGain 2.0 分轨标签。

.DESCRIPTION
    默认全量重扫（不再默认 -S），把外部下载 / 重封装文件自带的
    ReplayGain 元数据也统一到项目基准：-18 LUFS、ID3v2.3、true peak、
    峰值上限 -1 dBTP、总是启用削波保护。

    安全参数写在运行时生成的临时 preset 里（用完即删），不在仓库中
    新增 ini 文件，也不依赖 rsgain 安装目录里的预设内容。

    每个目录输出 replaygain.csv（-O），脚本末尾汇总最热的曲目、正增益
    曲目、以及被削波保护调整 / 原始峰值过 0 dB 的曲目，便于提前发现
    会把功放推到供电跌落边界的母带。

.PARAMETER MusicDir
    曲库根目录。省略时交互询问。

.PARAMETER Incremental
    增量模式：加 -S 跳过已有 ReplayGain 标签的文件。会保留外部来源的
    旧标签，基准可能不一致，一般只在 clear-cover.ps1 之后做快速补扫。

.PARAMETER Threads
    rsgain 并行线程数，默认 4（避免把 SD 卡读卡器打满）。

.PARAMETER RsgainPath
    显式指定 rsgain.exe 路径。

.EXAMPLE
    .\rsgain-scan.ps1 D:\Music
    .\rsgain-scan.ps1 D:\Music -Incremental -Threads 2
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$MusicDir,

    [switch]$Incremental,

    [ValidateRange(1, 32)]
    [int]$Threads = 4,

    [string]$RsgainPath
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$Message) {
    Write-Host $Message -ForegroundColor Cyan
}

# ---- 定位 rsgain ---------------------------------------------------------
$exe = $null
$candidates = @()
if ($RsgainPath) { $candidates += $RsgainPath }
$candidates += 'E:\tools\rsgain-3.7-win64\rsgain.exe'
foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $exe = $candidate
        break
    }
}
if (-not $exe) {
    $found = Get-Command rsgain -ErrorAction SilentlyContinue
    if ($found) { $exe = $found.Source }
}
if (-not $exe) {
    Write-Host '[ERROR] rsgain not found. Install via: scoop install extras/rsgain' -ForegroundColor Red
    Write-Host '        or pass -RsgainPath <path\to\rsgain.exe>' -ForegroundColor Red
    exit 1
}

# ---- 曲库目录 -----------------------------------------------------------
if (-not $MusicDir) {
    $MusicDir = Read-Host 'Enter music folder path, e.g. D:\Music'
}
if (-not (Test-Path -LiteralPath $MusicDir -PathType Container)) {
    Write-Host "[ERROR] folder not found: $MusicDir" -ForegroundColor Red
    exit 1
}
$MusicDir = (Resolve-Path -LiteralPath $MusicDir).Path

$modeText = if ($Incremental) { 'incremental (-S)' } else { 'FULL rescan' }
Write-Step 'ReplayGain 2.0 library scan'
Write-Host "  rsgain  : $exe"
Write-Host "  library : $MusicDir"
Write-Host "  mode    : $modeText"
Write-Host '  preset  : -18 LUFS / ID3v2.3 / true peak / max -1 dBTP / clip=a'
Write-Host ''
Write-Host '  Tip: run clear-cover.ps1 FIRST (it remuxes tags), then this script.' -ForegroundColor Yellow
if ($Incremental) {
    Write-Host '  [WARN] -Incremental keeps externally sourced tags; the library' -ForegroundColor Yellow
    Write-Host '          baseline may drift. Prefer a full rescan.' -ForegroundColor Yellow
}
Write-Host ''

# ---- 运行时 preset（临时文件，finally 清理）------------------------------
# -p 接受完整路径（已实测），因此无需改动 rsgain 安装目录里的
# presets\no_album.ini，也不需要在项目里新增配置文件。
$preset = Join-Path $env:TEMP ("rsgain-safe-{0}.ini" -f [guid]::NewGuid().ToString('N'))
[System.IO.File]::WriteAllLines($preset, [string[]]@(
    '[Global]',
    'TagMode=i',            # 扫描并写入 ReplayGain 2.0 标签
    'Album=false',          # 分类文件夹不是专辑，只算 track gain
    'TargetLoudness=-18',
    'ClipMode=a',           # 总是做削波保护（原 no_album.ini 只管正增益）
    'MaxPeakLevel=-1.0',    # 留 1 dB 余量，压低功放瞬时电流
    'TruePeak=true',        # 用 true peak，兜住 intersample 过冲
    'Lowercase=false',
    'ID3v2Version=3',       # 统一 ID3v2.3（clear-cover.ps1 也强制 v2.3）
    'OpusMode=d',
    'PreserveMtimes=true',
    'DualMono=false'
))

$exitCode = 0
try {
    $scanArgs = @('easy', '-p', $preset, '-O', '-m', $Threads.ToString())
    if ($Incremental) { $scanArgs += '-S' }
    $scanArgs += $MusicDir

    & $exe @scanArgs
    $rc = $LASTEXITCODE

    if ($rc -ne 0) {
        Write-Host "[ERROR] rsgain failed with exit code $rc" -ForegroundColor Red
        $exitCode = $rc
    }
    else {
        # ---- 汇总所有 replaygain.csv ------------------------------------
        $csvs = @(Get-ChildItem -LiteralPath $MusicDir -Filter 'replaygain.csv' -Recurse -File)
        $rows = @()
        $culture = [System.Globalization.CultureInfo]::InvariantCulture
        foreach ($csv in $csvs) {
            $lines = @(Get-Content -LiteralPath $csv.FullName)
            foreach ($line in ($lines | Select-Object -Skip 1)) {
                $c = $line -split "`t"
                if ($c.Count -lt 7) { continue }
                try {
                    $rows += [pscustomobject]@{
                        File     = $c[0]
                        LUFS     = [double]::Parse($c[1].Trim(), $culture)
                        Gain     = [double]::Parse($c[2].Trim(), $culture)
                        PeakDb   = [double]::Parse($c[4].Trim(), $culture)
                        PeakType = $c[5].Trim()
                        Clip     = $c[6].Trim()
                    }
                }
                catch {
                    # 空行 / 无法解析的行，忽略
                }
            }
        }

        Write-Host ''
        Write-Step ("Scan complete: {0} tracks in {1} CSV file(s)" -f $rows.Count, $csvs.Count)

        if ($rows.Count -gt 0) {
            Write-Host ''
            Write-Host 'Hottest 5 tracks (highest LUFS = heaviest sustained amp load):' -ForegroundColor Yellow
            foreach ($r in ($rows | Sort-Object LUFS -Descending | Select-Object -First 5)) {
                Write-Host ("  {0,8:N2} LUFS  gain {1,7:N2} dB  peak {2,6:N2} dB  {3}" -f
                            $r.LUFS, $r.Gain, $r.PeakDb, $r.File)
            }

            $positive = @($rows | Where-Object { $_.Gain -gt 0 })
            # PeakDb 是原始音频的 true peak；应用 ReplayGain 后的实际输出
            # 峰值是 PeakDb + Gain。只有它逼近 -1 dBTP（或真的被削波保护
            # 调整过）才值得警告 —— 热母带经负增益后其实很安全。
            $nearCeil = @($rows | Where-Object {
                $_.Clip -match '^Y' -or ($_.PeakDb + $_.Gain) -gt -1.0
            })
            Write-Host ''
            Write-Host ("Positive gain: {0}   Post-gain peak > -1 dBTP / clipping-adjusted: {1}" -f
                        $positive.Count, $nearCeil.Count)
            foreach ($r in ($nearCeil | Select-Object -First 10)) {
                Write-Host ("  [PEAK] post-gain {0,6:N2} dBTP  clip={1}  {2}" -f
                            ($r.PeakDb + $r.Gain), $r.Clip, $r.File)
            }
        }

        if ($csvs.Count -gt 0) {
            Write-Host ''
            Write-Host 'CSV reports:'
            foreach ($csv in $csvs) { Write-Host "  $($csv.FullName)" }
        }
        Write-Host ''
        Write-Step 'Done. Put the card back into the player.'
    }
}
finally {
    Remove-Item -LiteralPath $preset -Force -ErrorAction SilentlyContinue
}

exit $exitCode
