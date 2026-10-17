@echo off
cd /d "%~dp0"
echo This factory programming command erases all saved settings and other data on the selected ESP32.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Flash Firmware.ps1" -FactoryReset %*
pause
