param(
    [string]$Action = "",
    [string]$Port = ""
)

$Actions = @("build", "flash", "monitor", "menuconfig", "erase", "size", "clean", "reconfigure")

$Activate = 'C:\Users\OpenTankOfBeta\.espressif\activate.ps1'
if (-not (Test-Path $Activate)) {
    Write-Host "Regenerating IDF activate script ..." -ForegroundColor Yellow
    $Py = "C:\Users\OpenTankOfBeta\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe"
    if (-not (Test-Path $Py)) { $Py = "python" }
    $Out = & $Py "D:\esp\v6.1\esp-idf\tools\activate.py" --export
    if ($LASTEXITCODE -ne 0 -or -not $Out) {
        Write-Host "Failed to generate IDF activate script" -ForegroundColor Red
        Read-Host "Press Enter to exit"
        exit 1
    }
    Copy-Item $Out $Activate -Force
}

& $Activate | Out-Null
$env:IDF_TOOLS_PATH = 'C:\Users\OpenTankOfBeta\.espressif'

function Get-Port {
    $Ports = @((Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | Select-Object -ExpandProperty DeviceID))
    if (-not $Ports) {
        $Ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    }
    if (-not $Ports) {
        Write-Host "No serial port detected. Connect USB and install the driver." -ForegroundColor Red
        return ""
    }
    if ($Ports.Count -gt 1) {
        Write-Host "Available ports: $($Ports -join ', ')" -ForegroundColor Yellow
    }
    return $Ports[0]
}

function Show-Help {
    Write-Host "Usage:" -ForegroundColor Cyan
    Write-Host "  .\dev.ps1 [action] [-p COM3]"
    Write-Host "  .\dev.ps1                    interactive menu"
    Write-Host "  .\dev.ps1 <a>+<b>+...       chain multiple actions"
    Write-Host "Actions:" -ForegroundColor Cyan
    Write-Host "  build        compile firmware"
    Write-Host "  flash        flash (auto-detect port)"
    Write-Host "  monitor      serial monitor (Ctrl+] to exit)"
    Write-Host "  menuconfig   configuration menu"
    Write-Host "  erase        erase flash"
    Write-Host "  size         show firmware size"
    Write-Host "  clean        full clean"
    Write-Host "  reconfigure  rerun cmake (for Kconfig/component changes)"
    Write-Host "Examples:" -ForegroundColor Cyan
    Write-Host "  .\dev.ps1 build+flash"
    Write-Host "  .\dev.ps1 build+flash+monitor -p COM3"
}

function Show-Menu {
    Write-Host ""
    Write-Host "=== Xiaomiao Dev Tool ===" -ForegroundColor Cyan
    Write-Host " 1. Build"
    Write-Host " 2. Flash"
    Write-Host " 3. Monitor"
    Write-Host " 4. Menuconfig"
    Write-Host " 5. Erase flash"
    Write-Host " 6. Size"
    Write-Host " 7. Full clean"
    Write-Host " 8. Reconfigure (cmake)"
    Write-Host " 0. Exit"
}

function Run-Action([string]$Name) {
    switch ($Name) {
        "build"        { idf.py build }
        "reconfigure"  { idf.py reconfigure }
        "monitor"      { if (-not $Port) { $Port = Get-Port }; if ($Port) { idf.py -p $Port monitor } }
        "menuconfig"   { idf.py menuconfig }
        "erase"        { if (-not $Port) { $Port = Get-Port }; if ($Port) { idf.py -p $Port erase-flash } }
        "size"         { idf.py size }
        "clean"        { idf.py fullclean }
        "flash" {
            if (-not $Port) { $Port = Get-Port }
            if ($Port) {
                Write-Host "Flashing to $Port ..." -ForegroundColor Green
                idf.py -p $Port flash
            }
        }
    }
}

if (-not $Action) {
    if (-not [Console]::IsInputRedirected) {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    }
    while ($true) {
        Show-Menu
        $Choice = Read-Host "Select"
        if ($Choice -match '\+') {
            # chained choices, e.g. "1+2" or "1+2+3"
            $Done = $false
            foreach ($C in ($Choice -split '\+')) {
                switch ($C) {
                    "1" { Run-Action "build" }
                    "2" { Run-Action "flash" }
                    "3" { Run-Action "monitor" }
                    "4" { Run-Action "menuconfig" }
                    "5" { Run-Action "erase" }
                    "6" { Run-Action "size" }
                    "7" { Run-Action "clean" }
                    "8" { Run-Action "reconfigure" }
                    "0" { exit 0 }
                    default { Write-Host "Invalid choice: $C" -ForegroundColor Yellow }
                }
            }
            continue
        }
        switch ($Choice) {
            "1" { Run-Action "build" }
            "2" { Run-Action "flash" }
            "3" { Run-Action "monitor" }
            "4" { Run-Action "menuconfig" }
            "5" { Run-Action "erase" }
            "6" { Run-Action "size" }
            "7" { Run-Action "clean" }
            "8" { Run-Action "reconfigure" }
            "0" { exit 0 }
            default { Write-Host "Invalid choice" -ForegroundColor Yellow }
        }
    }
} elseif ($Action -eq "help" -or ($Action -notmatch '\+' -and $Actions -notcontains $Action)) {
    # help, or a single unknown token with no '+' separators
    Show-Help
    exit 1
} else {
    $Steps = $Action -split '\+'
    foreach ($Step in $Steps) {
        if ($Actions -notcontains $Step) {
            Write-Host "Unknown action: $Step" -ForegroundColor Red
            Show-Help
            exit 1
        }
    }
    foreach ($Step in $Steps) {
        Write-Host "`n>>> $Step" -ForegroundColor Cyan
        Run-Action $Step
        if ($LASTEXITCODE -ne 0) {
            Write-Host "Action '$Step' failed (exit $LASTEXITCODE)" -ForegroundColor Red
            exit $LASTEXITCODE
        }
    }
    exit 0
}