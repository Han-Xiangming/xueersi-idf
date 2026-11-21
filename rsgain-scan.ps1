#Requires -Version 5.1
<#
.SYNOPSIS
    扫描 SD 卡曲库并写入 ReplayGain 2.0 分轨标签，支持全量 / 智能增量 / 传统增量。

.DESCRIPTION
    三种模式：
      Full        全量重扫，统一外部下载 / 重封装文件自带的旧标签。
      Smart       智能增量：只重扫新增 / 修改 / 不在校验清单里的文件；
                  清单记录本脚本用同一 preset 成功扫描过的曲目。
      Incremental 传统 rsgain -S：跳过任何已有 ReplayGain 标签的文件，
                  可能保留外部旧基准，只作 clear-cover.ps1 后的快速补扫。

    曲库标签基准：-18 LUFS、ID3v2.3、true peak、峰值上限 -1 dBTP、
    总是启用削波保护。安全参数写在运行时生成的临时 preset 里（用完即删），
    不在仓库中新增 ini 文件，也不依赖 rsgain 安装目录里的预设内容。

    双击运行（本脚本或 rsgain-scan.bat）且不带参数时，会先询问曲库目录，
    再弹出模式菜单（默认 Smart）；命令行不带 -Mode 时仍默认 Full，
    兼容已有自动化调用。脚本结束会停在"按 Enter 关闭"提示上，
    从终端或自动化调用时不阻塞。

    每个目录输出 replaygain.csv（-O），脚本末尾汇总最热的曲目、正增益
    曲目、以及被削波保护调整 / 原始峰值过 0 dB 的曲目，便于提前发现
    会把功放推到供电跌落边界的母带。

.PARAMETER MusicDir
    曲库根目录。省略时交互询问。

.PARAMETER Mode
    扫描模式：Full / Smart / Incremental。不能与 -Full / -Smart / -Incremental 同用。

.PARAMETER Full
    全量重扫（命令行默认）。

.PARAMETER Smart
    智能增量：按曲库根目录的 .rsgain_scan_state.tsv 判断新增 / 修改 /
    外部标签文件，只重扫这些文件；成功后刷新清单。

.PARAMETER Incremental
    传统增量：加 -S 跳过已有 ReplayGain 标签的文件。会保留外部来源的
    旧标签，基准可能不一致，不更新智能清单。

.PARAMETER Threads
    rsgain 并行线程数，默认 4（避免把 SD 卡读卡器打满）。

.PARAMETER RsgainPath
    显式指定 rsgain.exe 路径。

.PARAMETER Pause
    结束时总是等待按 Enter，不自动判断启动方式。

.PARAMETER NoPause
    结束时直接退出，不等待按 Enter（自动化 / 脚本调用时使用）。

.EXAMPLE
    .\rsgain-scan.ps1 D:\Music
    .\rsgain-scan.ps1 D:\Music -Smart -Threads 2
    .\rsgain-scan.ps1 D:\Music -Mode Full
    .\rsgain-scan.ps1 D:\Music -Incremental
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$MusicDir,

    [ValidateSet('Full', 'Smart', 'Incremental')]
    [string]$Mode,

    [switch]$Full,

    [switch]$Smart,

    [switch]$Incremental,

    [ValidateRange(1, 32)]
    [int]$Threads = 4,

    [string]$RsgainPath,

    [switch]$Pause,

    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'

function Write-Step([string]$Message) {
    Write-Host $Message -ForegroundColor Cyan
}

# ---- 结束时的窗口停留 ----------------------------------------------------
# 双击运行（资源管理器里双击 .bat / .ps1）时，窗口会随脚本结束直接关闭，
# 看不到汇总结果。判定方式：从当前进程沿父进程链往上找，遇到的第一个
# "非 shell 类祖先"若是 explorer.exe，说明是用户在资源管理器里双击启动
# 的 —— 此时停在"按 Enter 关闭"上；从终端 / 自动化调用时不阻塞。
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
    Write-Host ''
    Write-Step 'All done. Press Enter to close this window.'
    try { [void](Read-Host) } catch { }
}

# ---- 模式选择 ------------------------------------------------------------
function Resolve-ScanMode {
    param(
        [string]$ExplicitMode,
        [bool]$HasFull,
        [bool]$HasIncremental,
        [bool]$HasSmart,
        [bool]$Interactive
    )
    $switches = @()
    if ($HasFull) { $switches += 'Full' }
    if ($HasIncremental) { $switches += 'Incremental' }
    if ($HasSmart) { $switches += 'Smart' }

    if ($ExplicitMode -and $switches.Count -gt 0) {
        throw 'Use either -Mode or a mode switch (-Full/-Incremental/-Smart), not both.'
    }
    if ($switches.Count -gt 1) {
        throw 'Only one of -Full, -Incremental, -Smart may be used.'
    }
    if ($ExplicitMode) { return $ExplicitMode }
    if ($switches.Count -eq 1) { return $switches[0] }
    if (-not $Interactive) { return 'Full' }

    Write-Host ''
    Write-Step 'Select scan mode:'
    Write-Host '  1) Smart incremental - rescan only new/changed files (recommended)'
    Write-Host '  2) Full rescan       - rescan every track'
    Write-Host '  3) Incremental       - rsgain -S, skip any file that already has a tag'
    $answer = Read-Host 'Enter 1 / 2 / 3 (default 1)'
    switch ($answer.Trim()) {
        '2' { return 'Full' }
        '3' { return 'Incremental' }
        default { return 'Smart' }
    }
}

# ---- 智能增量状态文件 helpers --------------------------------------------
# 状态文件只记录"本脚本用同一 preset 成功扫过"的曲目；若 preset 改了，
# 里面所有条目自动失效，Smart 会退化成一次全量重扫。
function Get-RelativePath([string]$FullName, [string]$Root) {
    $prefix = $Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($FullName.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        return $FullName.Substring($prefix.Length)
    }
    return [IO.Path]::GetFileName($FullName)
}

function Get-CurrentMusicFiles([string]$Root) {
    $supported = @('.mp3', '.flac', '.m4a', '.ogg', '.opus', '.wav', '.wma', '.mp2', '.aif', '.aiff')
    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
               Where-Object { $supported -contains $_.Extension.ToLowerInvariant() })
    $map = @{}
    foreach ($f in $files) {
        $rel = Get-RelativePath $f.FullName $Root
        $map[$rel.ToLowerInvariant()] = [pscustomobject]@{
            Relative = $rel
            FullName = $f.FullName
            Size     = $f.Length
            Mtime    = $f.LastWriteTimeUtc.Ticks
        }
    }
    return $map
}

function Read-ScanState([string]$Path, [string]$PresetHash) {
    $state = @{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $state }
    foreach ($line in @(Get-Content -LiteralPath $Path -Encoding UTF8 -ErrorAction SilentlyContinue)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        $c = $line -split "`t"
        if ($c.Count -lt 4 -or $c[3].Trim() -ne $PresetHash) { continue }
        try {
            $state[$c[0].Trim().ToLowerInvariant()] = [pscustomobject]@{
                Size  = [int64]$c[1].Trim()
                Mtime = [int64]$c[2].Trim()
            }
        } catch { }
    }
    return $state
}

function Write-ScanState([string]$Path, [string]$PresetHash, [hashtable]$Entries) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('# rsgain-scan state v1')
    $lines.Add("# preset $PresetHash")
    foreach ($key in @($Entries.Keys | Sort-Object)) {
        $e = $Entries[$key]
        $lines.Add(("{0}`t{1}`t{2}`t{3}" -f $e.Relative, $e.Size, $e.Mtime, $PresetHash))
    }
    # 先去掉 Hidden，否则第二次写入隐藏文件会被 Windows 拒绝。
    try {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            $item = Get-Item -LiteralPath $Path -Force
            if ($item.Attributes -band [IO.FileAttributes]::Hidden) {
                $item.Attributes = $item.Attributes -band (-bnot [IO.FileAttributes]::Hidden)
            }
        }
    } catch { }
    [IO.File]::WriteAllLines($Path, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false)))
    try {
        $item = Get-Item -LiteralPath $Path -Force
        $item.Attributes = $item.Attributes -bor [IO.FileAttributes]::Hidden
    } catch { }
}

function Remove-GainTags([string]$ExePath, [string[]]$Paths) {
    if ($Paths.Count -eq 0) { return $true }
    # custom 模式下 -p 是 --preserve-mtimes，不是 preset；
    # -s d 只删 ReplayGain 标签，不碰音频流。
    $batchSize = 50
    for ($i = 0; $i -lt $Paths.Count; $i += $batchSize) {
        $batch = @($Paths | Select-Object -Skip $i -First $batchSize)
        Write-Step ("Clearing old ReplayGain tags: {0}/{1}" -f ($i + $batch.Count), $Paths.Count)
        & $ExePath custom -s d -p @batch
        if ($LASTEXITCODE -ne 0) { return $false }
    }
    return $true
}

function Backup-ReplaygainCsvs([string]$Root) {
    $csvs = @(Get-ChildItem -LiteralPath $Root -Filter 'replaygain.csv' -Recurse -File -ErrorAction SilentlyContinue)
    if ($csvs.Count -eq 0) { return @() }
    $dir = Join-Path $env:TEMP ("rsgain-csv-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $backup = @()
    foreach ($csv in $csvs) {
        $dest = Join-Path $dir ("{0:D4}.csv" -f $backup.Count)
        Copy-Item -LiteralPath $csv.FullName -Destination $dest -Force
        $backup += [pscustomobject]@{ Original = $csv.FullName; Backup = $dest }
        Remove-Item -LiteralPath $csv.FullName -Force -ErrorAction SilentlyContinue
    }
    return $backup
}

function Restore-ReplaygainCsvs($Backup) {
    foreach ($b in $Backup) {
        if (Test-Path -LiteralPath $b.Backup -PathType Leaf) {
            Copy-Item -LiteralPath $b.Backup -Destination $b.Original -Force -ErrorAction SilentlyContinue
        }
    }
}

function Remove-CsvBackup($Backup) {
    if ($Backup.Count -gt 0) {
        $dir = Split-Path -Parent $Backup[0].Backup
        Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function Show-ScanSummary([string]$Root) {
    $csvs = @(Get-ChildItem -LiteralPath $Root -Filter 'replaygain.csv' -Recurse -File)
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
            } catch {
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
}

# ---- 定位 rsgain ---------------------------------------------------------
$exe = $null
$candidates = @()
if ($RsgainPath) { $candidates += $RsgainPath }
$candidates += 'E:\tools\rsgain-3.7-win64\rsgain.exe'
$candidates += 'D:\tools\rsgain-3.7-win64\rsgain.exe'
$candidates += "$env:USERPROFILE\scoop\shims\rsgain.exe"
$candidates += "$env:USERPROFILE\scoop\apps\rsgain\current\rsgain.exe"
foreach ($candidate in $candidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $exe = $candidate
        break
    }
}
if (-not $exe) {
    # 工具目录里再扫一遍（rsgain 可能装在别的版本目录 / 别的盘）
    foreach ($dir in @('E:\tools', 'D:\tools', "$env:USERPROFILE\scoop\apps")) {
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
        $hit = Get-ChildItem -LiteralPath $dir -Filter 'rsgain.exe' -Recurse -Depth 3 -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if ($hit) { $exe = $hit.FullName; break }
    }
}
if (-not $exe) {
    $found = Get-Command rsgain -ErrorAction SilentlyContinue
    if ($found) { $exe = $found.Source }
}
if (-not $exe) {
    Write-Host '[ERROR] rsgain not found. Install via: scoop install extras/rsgain' -ForegroundColor Red
    Write-Host '        or pass -RsgainPath <path\to\rsgain.exe>' -ForegroundColor Red
    if (Test-ShouldPause) { Wait-AtEnd }
    exit 1
}

# ---- 曲库目录 -----------------------------------------------------------
$musicDirProvided = [bool]$MusicDir
if (-not $MusicDir) {
    $MusicDir = Read-Host 'Enter music folder path, e.g. D:\Music'
}
if (-not (Test-Path -LiteralPath $MusicDir -PathType Container)) {
    Write-Host "[ERROR] folder not found: $MusicDir" -ForegroundColor Red
    if (Test-ShouldPause) { Wait-AtEnd }
    exit 1
}
$MusicDir = (Resolve-Path -LiteralPath $MusicDir).Path

try {
    $scanMode = Resolve-ScanMode -ExplicitMode $Mode -HasFull:$Full `
                                 -HasIncremental:$Incremental -HasSmart:$Smart `
                                 -Interactive:(-not $musicDirProvided)
} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    if (Test-ShouldPause) { Wait-AtEnd }
    exit 1
}

$modeText = switch ($scanMode) {
    'Full'        { 'FULL rescan' }
    'Smart'       { 'SMART incremental (verified-file manifest)' }
    'Incremental' { 'incremental (-S, skip tagged files)' }
}
Write-Step 'ReplayGain 2.0 library scan'
Write-Host "  rsgain  : $exe"
Write-Host "  library : $MusicDir"
Write-Host "  mode    : $modeText"
Write-Host '  preset  : -18 LUFS / ID3v2.3 / true peak / max -1 dBTP / clip=a'
Write-Host ''
Write-Host '  Tip: run clear-cover.ps1 FIRST (it remuxes tags), then this script.' -ForegroundColor Yellow
if ($scanMode -eq 'Smart') {
    Write-Host '  [INFO] Smart writes .rsgain_scan_state.tsv in the library root.' -ForegroundColor Yellow
    Write-Host '         New/changed files have old gain tags cleared before rescan.' -ForegroundColor Yellow
}
if ($scanMode -eq 'Incremental') {
    Write-Host '  [WARN] -Incremental keeps externally sourced tags; the library' -ForegroundColor Yellow
    Write-Host '          baseline may drift. Prefer -Smart or a full rescan.' -ForegroundColor Yellow
    Write-Host '          The smart manifest is not updated in this mode.' -ForegroundColor Yellow
}
Write-Host ''

# ---- 运行时 preset（临时文件，finally 清理）------------------------------
# -p 接受完整路径（已实测），因此无需改动 rsgain 安装目录里的
# presets\no_album.ini，也不需要在项目里新增配置文件。
$preset = Join-Path $env:TEMP ("rsgain-safe-{0}.ini" -f [guid]::NewGuid().ToString('N'))
$presetLines = [string[]]@(
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
)
$presetText = $presetLines -join "`n"
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $hashBytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($presetText))
}
finally {
    $sha.Dispose()
}
$presetHash = ([BitConverter]::ToString($hashBytes)).Replace('-', '').ToLowerInvariant()
[System.IO.File]::WriteAllLines($preset, $presetLines)

$stateFile = Join-Path $MusicDir '.rsgain_scan_state.tsv'
$csvBackup = @()
$exitCode = 0
try {
    $skipScan = $false
    $pendingFiles = @()

    if ($scanMode -eq 'Smart') {
        $current = Get-CurrentMusicFiles $MusicDir
        $state = Read-ScanState $stateFile $presetHash
        foreach ($key in $current.Keys) {
            $e = $current[$key]
            $old = $state[$key]
            if (-not $old -or $old.Size -ne $e.Size -or $old.Mtime -ne $e.Mtime) {
                $pendingFiles += $e
            }
        }
        Write-Host ("  smart   : {0} verified, {1} need scan" -f
                    ($current.Count - $pendingFiles.Count), $pendingFiles.Count)
        if ($pendingFiles.Count -eq 0) { $skipScan = $true }
    }

    # 清掉旧 CSV，避免上次结果混进本次汇总；失败时再还原。
    $csvBackup = @(Backup-ReplaygainCsvs $MusicDir)

    if ($skipScan) {
        Restore-ReplaygainCsvs $csvBackup
        Write-Host ''
        Write-Step 'Nothing changed since last verified scan. Done.'
    }
    else {
        # Smart 待扫文件可能带着外部旧标签；先删标，再用 -S 让 rsgain 跳过
        # 清单中未变的文件，只重扫这批文件。
        if ($scanMode -eq 'Smart' -and -not (Remove-GainTags $exe @($pendingFiles.FullName))) {
            throw 'Failed to clear old ReplayGain tags; the smart state file was not updated.'
        }

        $scanArgs = @('easy', '-p', $preset, '-O', '-m', $Threads.ToString())
        if ($scanMode -ne 'Full') { $scanArgs += '-S' }
        $scanArgs += $MusicDir

        & $exe @scanArgs
        $rc = $LASTEXITCODE

        if ($rc -ne 0) {
            Restore-ReplaygainCsvs $csvBackup
            Write-Host "[ERROR] rsgain failed with exit code $rc" -ForegroundColor Red
            $exitCode = $rc
        }
        else {
            Remove-CsvBackup $csvBackup
            if ($scanMode -eq 'Incremental') {
                Write-Host '  note: legacy incremental does not update the smart manifest.' -ForegroundColor Yellow
            }
            else {
                $verified = Get-CurrentMusicFiles $MusicDir
                Write-ScanState $stateFile $presetHash $verified
                Write-Host ("  state   : {0}" -f $stateFile)
            }
            Show-ScanSummary $MusicDir
            Write-Host ''
            Write-Step 'Done. Put the card back into the player.'
        }
    }
}
catch {
    Restore-ReplaygainCsvs $csvBackup
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    $exitCode = 1
}
finally {
    Remove-CsvBackup $csvBackup
    Remove-Item -LiteralPath $preset -Force -ErrorAction SilentlyContinue
}

if (Test-ShouldPause) {
    Wait-AtEnd
}

exit $exitCode
