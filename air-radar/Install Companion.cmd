@echo off
cd /d "%~dp0"
py -3 -m venv .venv
if errorlevel 1 goto fail
".venv\Scripts\python.exe" -m pip install -r tools\requirements.txt
if errorlevel 1 goto fail
echo Companion installed. Open Start Setup.cmd with the radar connected.
pause
exit /b 0
:fail
echo Install Python 3.10 or newer from python.org and check your internet connection.
pause
exit /b 1
