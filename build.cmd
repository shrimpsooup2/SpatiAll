@echo off
rem Configure + build Spatiall. Usage: build.cmd [Release|Debug]
rem   SPATIALL_BUILD_DIR  build tree root (default: .\build); ~1 GB of downloads + objects
rem   SPATIALL_VSDEVCMD   path to VsDevCmd.bat when vswhere can't find Visual Studio
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

where cl >nul 2>nul && goto :toolchain_ready
set VSDEV=%SPATIALL_VSDEVCMD%
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not defined VSDEV if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDEV=%%i\Common7\Tools\VsDevCmd.bat
if not defined VSDEV (
  echo Visual Studio C++ build tools not found. Install "Desktop development with C++" or set SPATIALL_VSDEVCMD.
  exit /b 1
)
call "%VSDEV%" -arch=x64 -host_arch=x64 >nul || exit /b 1
:toolchain_ready

set BUILD_ROOT=%SPATIALL_BUILD_DIR%
if "%BUILD_ROOT%"=="" set BUILD_ROOT=%~dp0build
cmake -S "%~dp0." -B "%BUILD_ROOT%\%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DFETCHCONTENT_BASE_DIR="%BUILD_ROOT%\_deps" || exit /b 1
cmake --build "%BUILD_ROOT%\%CONFIG%" || exit /b 1
echo Built: %BUILD_ROOT%\%CONFIG%\Spatiall.exe
