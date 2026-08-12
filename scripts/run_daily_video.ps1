param(
    [string]$Device = 'http://esp32cam.local',
    [string]$StoreName = '',
    [string]$StoreRegion = '',
    [string]$PythonExe = 'python'
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$logDirectory = Join-Path $projectRoot 'output\task-logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$logPath = Join-Path $logDirectory ((Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$pythonArguments = @((Join-Path $PSScriptRoot 'run_daily_video.py'), '--device', $Device)
if (-not [string]::IsNullOrWhiteSpace($StoreName)) {
    $pythonArguments += @('--store-name', $StoreName.Trim())
}
if (-not [string]::IsNullOrWhiteSpace($StoreRegion)) {
    $pythonArguments += @('--store-region', $StoreRegion.Trim())
}
& $PythonExe @pythonArguments *>&1 |
    Tee-Object -LiteralPath $logPath
exit $LASTEXITCODE
