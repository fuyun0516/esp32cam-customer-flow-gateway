param(
    [string]$Device = 'http://esp32cam.local',
    [string]$RunAt = '00:20',
    [string]$StoreName = '',
    [string]$StoreRegion = '',
    [string]$PythonExe = 'python',
    [ValidateRange(1, 30)][int]$LookbackDays = 7
)

$taskName = 'ESP32-CAM Daily Store Video'
$runner = Join-Path $PSScriptRoot 'run_daily_video.ps1'
if ([System.IO.Path]::IsPathRooted($PythonExe)) {
    if (-not (Test-Path -LiteralPath $PythonExe)) {
        throw "Python executable not found: $PythonExe"
    }
} elseif ($null -eq (Get-Command $PythonExe -ErrorAction SilentlyContinue)) {
    throw "Python command not found: $PythonExe"
}
$time = [DateTime]::ParseExact($RunAt, 'HH:mm', $null)
$arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$runner`" -Device `"$Device`" -PythonExe `"$PythonExe`" -LookbackDays $LookbackDays"
if (-not [string]::IsNullOrWhiteSpace($StoreName)) {
    if ($StoreName.Contains('"')) {
        throw 'StoreName cannot contain a double quote.'
    }
    $arguments += " -StoreName `"$($StoreName.Trim())`""
}
if (-not [string]::IsNullOrWhiteSpace($StoreRegion)) {
    if ($StoreRegion.Contains('"')) {
        throw 'StoreRegion cannot contain a double quote.'
    }
    $arguments += " -StoreRegion `"$($StoreRegion.Trim())`""
}
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arguments
$trigger = New-ScheduledTaskTrigger -Daily -At $time
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -RestartCount 96 -RestartInterval (New-TimeSpan -Minutes 15) -ExecutionTimeLimit (New-TimeSpan -Hours 4) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Create pending ESP32-CAM daily videos automatically; retry until the store LAN becomes available.' -Force
Get-ScheduledTask -TaskName $taskName
