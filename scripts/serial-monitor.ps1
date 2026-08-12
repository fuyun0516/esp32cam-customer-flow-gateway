param(
  [string]$PortName = '',
  [string]$LogPath = (Join-Path $PSScriptRoot '..\tmp\serial-live.log')
)

if ([string]::IsNullOrWhiteSpace($PortName)) {
  $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
  if ($ports.Count -ne 1) {
    throw "Unable to choose a serial port automatically. Available ports: $($ports -join ', '). Pass -PortName explicitly."
  }
  $PortName = $ports[0]
}

$serial = [System.IO.Ports.SerialPort]::new(
  $PortName,
  115200,
  [System.IO.Ports.Parity]::None,
  8,
  [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 250
$serial.DtrEnable = $false
$serial.RtsEnable = $false

$logDirectory = Split-Path -Parent $LogPath
[System.IO.Directory]::CreateDirectory($logDirectory) | Out-Null
$writer = [System.IO.StreamWriter]::new($LogPath, $false, [System.Text.UTF8Encoding]::new($false))
$writer.AutoFlush = $true

try {
  $serial.Open()
  while ($true) {
    try {
      $writer.WriteLine($serial.ReadLine())
    } catch [System.TimeoutException] {
    }
  }
} finally {
  if ($serial.IsOpen) {
    $serial.Close()
  }
  $writer.Dispose()
  $serial.Dispose()
}
