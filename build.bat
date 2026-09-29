@echo off
setlocal
cd /d "%~dp0"
rem Builds FRSModLoader (x86) and installs it into the NFSU2 scripts\ folder.
rem
rem   build.bat            build and install
rem   build.bat nomod      build only

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere not found
  exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * ^
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
  -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [error] Visual Studio with the C++ toolset not found
  exit /b 1
)

set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

if not exist "third_party\cef\include\cef_version.h" (
  echo [..] downloading CEF ^(first time only^)
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_cef.ps1
  if errorlevel 1 exit /b 1
)

if not exist "build\CMakeCache.txt" (
  "%CMAKE%" --preset x86
  if errorlevel 1 exit /b 1
)

"%CMAKE%" --build --preset x86
if errorlevel 1 (
  echo [error] build failed
  exit /b 1
)

if /i "%1"=="nomod" goto :done

powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1
if errorlevel 1 exit /b 1

:done
echo [ok] done
