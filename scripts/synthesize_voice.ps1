param(
    [Parameter(Mandatory = $true)][string]$TextPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$text = Get-Content -LiteralPath $TextPath -Raw -Encoding UTF8
$voice = New-Object -ComObject SAPI.SpVoice
$preferred = $voice.GetVoices() | Where-Object {
    $_.GetDescription() -like '*Huihui*' -or $_.GetDescription() -like '*Yaoyao*'
} | Select-Object -First 1
if ($null -ne $preferred) {
    $voice.Voice = $preferred
}
$stream = New-Object -ComObject SAPI.SpFileStream
$stream.Open($OutputPath, 3, $false)
$voice.AudioOutputStream = $stream
$voice.Rate = 1
$voice.Volume = 100
[void]$voice.Speak($text, 0)
$stream.Close()
Get-Item -LiteralPath $OutputPath | Select-Object FullName, Length
