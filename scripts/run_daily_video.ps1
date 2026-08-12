param(
    [string]$Device = 'http://esp32cam.local',
    [string]$StoreName = '',
    [string]$StoreRegion = '',
    [string]$PythonExe = 'python',
    [ValidateRange(1, 30)][int]$LookbackDays = 7
)

$projectRoot = Split-Path -Parent $PSScriptRoot
$logDirectory = Join-Path $projectRoot 'output\task-logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$logPath = Join-Path $logDirectory ((Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
$runner = Join-Path $PSScriptRoot 'run_daily_video.py'
$pendingDays = [System.Collections.Generic.List[string]]::new()

for ($offset = $LookbackDays; $offset -ge 1; $offset--) {
    $day = (Get-Date).Date.AddDays(-$offset).ToString('yyyyMMdd')
    $dayOutput = Join-Path $projectRoot ("output\$day")
    $statusPath = Join-Path $dayOutput 'daily-task-status.json'
    $completed = $false

    if (Test-Path -LiteralPath $statusPath) {
        try {
            $status = Get-Content -LiteralPath $statusPath -Raw -Encoding UTF8 |
                ConvertFrom-Json
            if ($status.result -eq 'complete') {
                $completed = $status.video -and (Test-Path -LiteralPath $status.video)
            } elseif ([string]$status.result -like 'skipped:*') {
                $completed = $true
            }
        } catch {
            $completed = $false
        }
    }

    if (-not $completed) {
        $pendingDays.Add($day)
    }
}

if ($pendingDays.Count -eq 0) {
    "$(Get-Date -Format s) No pending completed days in the last $LookbackDays days." |
        Tee-Object -LiteralPath $logPath
    exit 0
}

foreach ($day in $pendingDays) {
    "$(Get-Date -Format s) Processing pending day $day" |
        Tee-Object -LiteralPath $logPath -Append
    $pythonArguments = @($runner, '--device', $Device, '--day', $day)
    if (-not [string]::IsNullOrWhiteSpace($StoreName)) {
        $pythonArguments += @('--store-name', $StoreName.Trim())
    }
    if (-not [string]::IsNullOrWhiteSpace($StoreRegion)) {
        $pythonArguments += @('--store-region', $StoreRegion.Trim())
    }
    & $PythonExe @pythonArguments *>&1 |
        Tee-Object -LiteralPath $logPath -Append
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

exit 0
