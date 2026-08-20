@echo off
REM Double-click this to install / repair / reset the Tapioca embedded-Python runtime.
REM It just launches the PowerShell menu with the right execution policy.
REM IMPORTANT: close Archicad before Repair or Reset (Windows locks loaded packages).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0EvP-Environment.ps1" %*
echo.
pause
