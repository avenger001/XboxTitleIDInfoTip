@echo off
setlocal enableextensions
set "SCRIPT_DIR=%~dp0"
set "SRC=%SCRIPT_DIR%XboxTitleIdInfoTip.cpp"
set "OUTDIR=%SCRIPT_DIR%build"
set "OBJDIR=%OUTDIR%\obj"
set "OBJ=%OBJDIR%\XboxTitleIdInfoTip.obj"
set "DLL=%OUTDIR%\XboxTitleIdInfoTip.dll"

rem --- Create build directories ---
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

rem --- Locate VS 2022 toolchain ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if defined VSROOT (
  call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
) else (
  where cl >nul 2>&1
  if errorlevel 1 (
    echo [ERROR] MSVC not found. Run from "x64 Native Tools Command Prompt for VS 2022".
    exit /b 1
  )
)

echo.
echo [BUILD] Compiling to OBJ...
rem We are now compiling for a DLL with the /LD flag.
cl /c /LD /EHsc /permissive- /std:c++17 /DUNICODE /D_UNICODE "%SRC%" /Fo"%OBJ%"
if errorlevel 1 (
  echo [ERROR] Compilation failed.
  exit /b 1
)

echo.
echo [BUILD] Linking DLL (x64)...
rem The linker will find the exports in the OBJ file thanks to the #pragma directives.
link /nologo /DLL /MACHINE:X64 /OUT:"%DLL%" ^
  "%OBJ%" shlwapi.lib ole32.lib uuid.lib advapi32.lib shell32.lib user32.lib
if errorlevel 1 (
  echo [ERROR] Linking failed.
  exit /b 1
)

echo.
echo [OK] Build complete.
exit /b 0