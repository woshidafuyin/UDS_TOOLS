@echo off
setlocal
cd /d "%~dp0"

echo [UDS Tool] Checking build environment...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -ValidateEnvironmentOnly
if errorlevel 1 goto :failed

if /I "%~1"=="check" goto :check_passed

echo.
echo [UDS Tool] Building dist and release package...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\package.ps1"
if errorlevel 1 goto :failed

echo.
echo [UDS Tool] Deployment package is ready:
echo   %~dp0UDS_Tool_Release.zip
goto :succeeded

:check_passed
echo.
echo [UDS Tool] Environment check passed. No files were built or deployed.

:succeeded
echo.
echo Press any key to close this window.
pause >nul
exit /b 0

:failed
echo.
echo [UDS Tool] Stopped. Read the specific error above.
echo Close any running copy from dist before retrying. If a build tool is missing,
echo install it or run scripts\build.ps1 with the path override shown above.
echo.
echo Press any key to close this window.
pause >nul
exit /b 1
