param([string]$Port='', [switch]$FactoryReset)
$ErrorActionPreference='Stop'
$radarCandidates=@((Join-Path $PSScriptRoot '.venv\Scripts\python.exe'),(Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'))
$radarPython=$radarCandidates | Where-Object {Test-Path -LiteralPath $_} | Select-Object -First 1
if(-not $radarPython){$radarCommand=Get-Command python.exe -ErrorAction SilentlyContinue;if($radarCommand){$radarPython=$radarCommand.Source}}
if(-not $radarPython){throw 'Install Python 3.10+, pyserial 3.5 and esptool 4.5.1 first.'}
$radarArguments=@((Join-Path $PSScriptRoot 'tools\flash_firmware.py'))
if($Port){$radarArguments+=@('--port',$Port)}
if($FactoryReset){$radarArguments+='--factory-reset'}
& $radarPython @radarArguments
exit $LASTEXITCODE
