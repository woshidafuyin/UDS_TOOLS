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
echo [UDS Tool] Stopped. Read the error above, install the missing component,
echo or run scripts\build.ps1 with -VisualStudioRoot, -CMakePath, or -QtRoot.
echo.
echo Press any key to close this window.
pause >nul
exit /b 1
