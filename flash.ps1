param(
    [string]$Port = "",
    [switch]$Monitor
)

$Activate = 'C:\Users\OpenTankOfBeta\.espressif\activate.ps1'
if (-not (Test-Path $Activate)) {
    Write-Host "Regenerating IDF activate script ..."
    $Py = "C:\Users\OpenTankOfBeta\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe"
    if (-not (Test-Path $Py)) { $Py = "python" }
    $Out = & $Py "D:\esp\v6.1-beta1\tools\activate.py" --export
    if ($LASTEXITCODE -ne 0 -or -not $Out) {
        Write-Error "Failed to generate IDF activate script"
        exit 1
    }
    Copy-Item $Out $Activate -Force
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