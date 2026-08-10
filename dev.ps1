param(
    [string]$Action = "",
    [string]$Port = ""
)

$Actions = @("build", "flash", "monitor", "menuconfig", "erase", "size", "clean")

$Activate = "C:\Users\OPENTA~1\AppData\Local\Temp\esp_idf_activate_OpenTankOfBeta\activate_x92aah2g.ps1"
if (-not (Test-Path $Activate)) {
    Write-Host "IDF activate script not found: $Activate" -ForegroundColor Red
    Read-Host "Press Enter to exit"
    exit 1
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
    Write-Host "  .\dev.ps1                  interactive menu"
    Write-Host "Actions:" -ForegroundColor Cyan
    Write-Host "  build        compile firmware"
    Write-Host "  flash        flash (auto-detect port)"
    Write-Host "  monitor      serial monitor (Ctrl+] to exit)"
    Write-Host "  menuconfig   configuration menu"
    Write-Host "  erase        erase flash"
    Write-Host "  size         show firmware size"
    Write-Host "  clean        full clean"
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
    Write-Host " 0. Exit"
}

function Run-Action([string]$Name) {
    switch ($Name) {
        "build"     { idf.py build }
        "monitor"   { if (-not $Port) { $Port = Get-Port }; if ($Port) { idf.py -p $Port monitor } }
        "menuconfig"{ idf.py menuconfig }
        "erase"     { if (-not $Port) { $Port = Get-Port }; if ($Port) { idf.py -p $Port erase-flash } }
        "size"      { idf.py size }
        "clean"     { idf.py fullclean }
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
        switch ($Choice) {
            "1" { Run-Action "build" }
            "2" { Run-Action "flash" }
            "3" { Run-Action "monitor" }
            "4" { Run-Action "menuconfig" }
            "5" { Run-Action "erase" }
            "6" { Run-Action "size" }
            "7" { Run-Action "clean" }
            "0" { exit 0 }
            default { Write-Host "Invalid choice" -ForegroundColor Yellow }
        }
    }
} else {
    if ($Action -eq "help" -or $Actions -notcontains $Action) {
        Show-Help
        exit 1
    }
    Run-Action $Action
    exit $LASTEXITCODE
}