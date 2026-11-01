<#
  One command to use any calibration tool. From this folder, in PowerShell:

    .\cal.ps1                  list the tools
    .\cal.ps1 ina226           flash it, then open its serial monitor
    .\cal.ps1 ina226 -Monitor  only open the monitor (it is already flashed)
    .\cal.ps1 ina226 -Build    only compile it, nothing is connected to

  COM ports are in common.ini. Close the monitor with Ctrl+C.
  Build output goes to the temp folder, so it never lands in OneDrive.
#>
param(
  [Parameter(Position = 0)][string]$Tool = '',
  [switch]$Monitor,
  [switch]$Build
)

$tools = [ordered]@{
  'ina226'     = @{ dir = 'ESP1\INA226';        board = 'ESP1'; what = 'solar, main battery and secondary INA226' }
  'calstore'   = @{ dir = 'ESP1\CalStore';      board = 'ESP1'; what = 'store results in ESP1, see what is stored' }
  'opto'       = @{ dir = 'ESP1\BatteryOpto';   board = 'ESP1'; what = 'fallback battery voltage (opto ADC)' }
  'acs758'     = @{ dir = 'ESP1\ACS758';        board = 'ESP1'; what = 'fallback battery current (ACS758 + ADS1115)' }
  'flow'       = @{ dir = 'ESP2\Flow';          board = 'ESP2'; what = 'flow K-factors and pump rates. RUNS PUMPS' }
  'pinfinder'  = @{ dir = 'ESP2\FlowPinFinder'; board = 'ESP2'; what = 'which flow sensor is on which GPIO' }
  'ph'         = @{ dir = 'ESP2\pH';            board = 'ESP2'; what = 'pH probe' }
  'ec'         = @{ dir = 'ESP2\EC';            board = 'ESP2'; what = 'EC probe' }
  'acs712'     = @{ dir = 'ESP2\ACS712';        board = 'ESP2'; what = 'mixer current. RUNS THE MIXER' }
  'soil'       = @{ dir = 'Nano\Soil';          board = 'Nano'; what = 'soil moisture dry and wet readings' }
  'ultrasonic' = @{ dir = 'Nano\Ultrasonic';    board = 'Nano'; what = 'tank empty and full distances' }
  'resflow'    = @{ dir = 'Nano\Flow';          board = 'Nano'; what = 'reservoir flow K-factor' }
  'dht22'      = @{ dir = 'Nano\DHT22';         board = 'Nano'; what = 'temperature and humidity offsets' }
  'bh1750'     = @{ dir = 'Nano\BH1750';        board = 'Nano'; what = 'light offset' }
  'npk'        = @{ dir = 'Nano\NPK';           board = 'Nano'; what = 'NPK sensor check, guarded trim' }
}

if ($Tool -eq '' -or -not $tools.Contains($Tool.ToLower())) {
  if ($Tool -ne '') { Write-Host "No tool called '$Tool'." -ForegroundColor Yellow }
  Write-Host "Usage: .\cal.ps1 <tool> [-Monitor] [-Build]`n"
  foreach ($k in $tools.Keys) { '  {0,-11} {1,-5} {2}' -f $k, $tools[$k].board, $tools[$k].what }
  exit 0
}

$pio = Join-Path $HOME '.platformio\penv\Scripts\pio.exe'
if (-not (Test-Path $pio)) { Write-Host "PlatformIO not found at $pio" -ForegroundColor Red; exit 1 }

$t = $tools[$Tool.ToLower()]
$dir = Join-Path $PSScriptRoot $t.dir
$env:PLATFORMIO_BUILD_DIR = Join-Path $env:TEMP ("smart-irrigation-cal\" + $Tool.ToLower())
$env:PLATFORMIO_LIBDEPS_DIR = Join-Path $env:TEMP ("smart-irrigation-cal\libdeps-" + $Tool.ToLower())

if ($t.board -eq 'Nano' -and -not $Monitor) { Write-Host 'Nano: disconnect D0/D1 (the link to ESP1) before flashing.' -ForegroundColor Yellow }
if ($t.board -ne 'Nano' -and -not $Monitor) { Write-Host "Flashing this to $($t.board) replaces its firmware until you flash that back." -ForegroundColor Yellow }

if ($Build) {
  & $pio run -d $dir
  exit $LASTEXITCODE
}
if (-not $Monitor) {
  & $pio run -d $dir -t upload
  if ($LASTEXITCODE -ne 0) { Write-Host 'Flash failed. Check the COM port in common.ini.' -ForegroundColor Red; exit $LASTEXITCODE }
}
& $pio device monitor -d $dir
