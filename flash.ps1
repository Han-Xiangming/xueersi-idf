param(
    [string]$Port = "",
    [switch]$Monitor
)

$Activate = "C:\Users\OPENTA~1\AppData\Local\Temp\esp_idf_activate_OpenTankOfBeta\activate_x92aah2g.ps1"
if (-not (Test-Path $Activate)) {
    Write-Error "IDF activate script not found: $Activate"
    exit 1
}

& $Activate | Out-Null
$env:IDF_TOOLS_PATH = 'C:\Users\OpenTankOfBeta\.espressif'

if (-not $Port) {
    $Ports = @((Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | Select-Object -ExpandProperty DeviceID))
    if (-not $Ports) {
        $Ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    }
    if (-not $Ports) {
        Write-Error "No serial port detected. Connect the USB cable and install the driver."
        exit 1
    }
    $Port = $Ports[0]
    Write-Host "Auto-selected port: $Port"
}

Write-Host "Flashing to $Port ..."
if ($Monitor) {
    idf.py -p $Port flash monitor
} else {
    idf.py -p $Port flash
}
exit $LASTEXITCODE