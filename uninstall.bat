@echo off
setlocal enableextensions
set "DLL_DST=%SystemRoot%\System32\XboxTitleIdInfoTip.dll"
set "MAP_DST=%SystemRoot%\System32\XboxTitleIDs.txt"

rem --- Elevation check ---
net session >nul 2>&1
if not %ERRORLEVEL%==0 (
  echo Requesting administrator privileges...
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Start-Process -Verb RunAs -FilePath '%~f0' -ArgumentList '%*'"
  exit /b
)

echo.
echo [UNINSTALL] Unregistering shell extension...
if exist "%DLL_DST%" "%SystemRoot%\System32\regsvr32.exe" /u /s "%DLL_DST%" >nul
if errorlevel 1 (
  echo [WARN] Failed to unregister DLL. It may not have been registered.
)

echo [UNINSTALL] Deleting shell extension hooks...
set "IQI={00021500-0000-0000-C000-000000000046}"
rem Remove from all file types (new in updated version)
reg delete "HKCR\*\shellex\%IQI%"         /f >nul 2>&1
reg delete "HKCR\Folder\shellex\%IQI%"    /f >nul 2>&1
reg delete "HKCR\Directory\shellex\%IQI%" /f >nul 2>&1
rem Deleting hooks in HKCU as well, just in case
reg delete "HKCU\Software\Classes\*\shellex\%IQI%"         /f >nul 2>&1
reg delete "HKCU\Software\Classes\Folder\shellex\%IQI%"    /f >nul 2>&1
reg delete "HKCU\Software\Classes\Directory\shellex\%IQI%" /f >nul 2>&1

rem Remove CLSID registration
reg delete "HKCR\CLSID\{A7C2C6B9-1B52-4E1E-9D56-2D2A9AB7D0C4}" /f >nul 2>&1

rem Remove from approved list
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved" /v "{A7C2C6B9-1B52-4E1E-9D56-2D2A9AB7D0C4}" /f >nul 2>&1

echo [UNINSTALL] Deleting DLL from System32...
if exist "%DLL_DST%" del /F /Q "%DLL_DST%"
if errorlevel 1 (
  echo [ERROR] Failed to delete DLL. It may be in use.
  echo Close all Explorer windows and try again.
)

echo.
choice /C YN /N /M "Remove mapping file %MAP_DST% ? (Y/N): "
if errorlevel 2 (
  echo [INFO] Keeping mapping file.
) else (
  if exist "%MAP_DST%" del /F /Q "%MAP_DST%"
)

echo.
echo [UNINSTALL] Restarting Explorer...
taskkill /F /IM explorer.exe >nul 2>&1
start explorer.exe

echo.
echo [OK] Uninstall complete.
exit /b 0