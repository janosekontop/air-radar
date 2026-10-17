param([string]$Port='', [ValidateRange(1024,65535)][int]$HttpPort=8765)
$ErrorActionPreference='Stop'
$radarCandidates=@((Join-Path $PSScriptRoot '.venv\Scripts\python.exe'),(Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'))
$radarPython=$radarCandidates | Where-Object {Test-Path -LiteralPath $_} | Select-Object -First 1
if(-not $radarPython){$radarCommand=Get-Command python.exe -ErrorAction SilentlyContinue;if($radarCommand){$radarPython=$radarCommand.Source}}
if(-not $radarPython){throw 'Install Python 3.10 or newer from python.org, then run Install Companion.cmd.'}
& $radarPython -c 'import serial' 2>$null
if($LASTEXITCODE -ne 0){throw 'Run Install Companion.cmd once, then start setup again.'}
$radarArguments=@((Join-Path $PSScriptRoot 'tools\wifi_setup.py'),'--http-port',$HttpPort,'--open-browser')
if($Port){$radarArguments+=@('--port',$Port)}
Write-Host 'The setup page stays on this computer. Close other serial monitors.'
Write-Host 'For USB data, keep this companion running. For direct Wi-Fi, close it after setup.'
& $radarPython @radarArguments
exit $LASTEXITCODE
