@echo off
setlocal enableextensions
set "SCRIPT_DIR=%~dp0"
set "OUTDIR=%SCRIPT_DIR%build"
set "DLL_SRC=%OUTDIR%\XboxTitleIdInfoTip.dll"
set "DLL_DST=%SystemRoot%\System32\XboxTitleIdInfoTip.dll"
set "MAP_DST=%SystemRoot%\System32\XboxTitleIDs.txt"
set "MAP_SRC=%~1"

rem --- Elevation check ---
net session >nul 2>&1
if not %ERRORLEVEL%==0 (
  echo Requesting administrator privileges...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath '%~f0' -ArgumentList '%*'"
  exit /b
)

rem --- Ensure DLL exists (build if missing) ---
if not exist "%DLL_SRC%" (
  echo(
  echo [INFO] DLL not found in build\. Attempting to build...
  call "%SCRIPT_DIR%build.bat"
  if errorlevel 1 (
    echo [ERROR] Build failed. Aborting install.
    exit /b 1
  )
)

echo(
echo [INSTALL] Copying DLL to System32...
copy /Y "%DLL_SRC%" "%DLL_DST%" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy DLL to "%DLL_DST%".
  exit /b 1
)

rem --- Mapping file handling ---
if "%MAP_SRC%"=="" goto :maybeCreateDefault
if not exist "%MAP_SRC%" (
  echo(
  echo [WARN] Mapping file "%MAP_SRC%" not found. Skipping copy.
  goto :register
)

echo(
echo [INSTALL] Copying mapping file to System32...
copy /Y "%MAP_SRC%" "%MAP_DST%" >nul
if errorlevel 1 (
  echo [ERROR] Failed to copy mapping file to "%MAP_DST%".
  exit /b 1
)
goto :register

:maybeCreateDefault
if not exist "%MAP_DST%" (
  echo(
  echo [INSTALL] Creating default mapping file at "%MAP_DST%" ...
  > "%MAP_DST%" echo 00000001=Sample Title
)

:register
echo(
echo [REGISTER] Unregistering (refresh)...
"%SystemRoot%\System32\regsvr32.exe" /u /s "%DLL_DST%" >nul

echo [REGISTER] Registering DLL...
"%SystemRoot%\System32\regsvr32.exe" "%DLL_DST%"
if errorlevel 1 (
  echo(
  echo [ERROR] regsvr32 failed. The DllRegisterServer entry point was not found.
  echo Try running from a Command Prompt with administrator privileges.
  echo For a detailed error, run:
  echo    "%SystemRoot%\System32\regsvr32.exe" "%DLL_DST%"
  exit /b 1
)

echo(
echo [INFO] Restarting Explorer to apply changes...
taskkill /F /IM explorer.exe >nul 2>&1
start explorer.exe

echo(
echo [OK] Installation complete.
exit /b 0
