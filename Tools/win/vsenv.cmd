@echo off
rem Sets up the x64 MSVC + Windows SDK environment, puts VS's CMake/Ninja on PATH, then runs %*.
rem Note: no parenthesized blocks below that expand %ProgramFiles(x86)% (its ")" would end the block).

if defined VSROOT goto havevs
rem Any VS 2022 edition (Community/Professional/Enterprise/BuildTools) that has the x64 MSVC tools.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -version [17.0^,18.0^) -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
:havevs
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" goto novs

rem Newest installed Windows 10/11 SDK that actually contains the UCRT headers. Passing it to
rem vcvars explicitly avoids relying on winsdk.bat's registry probing.
set "SOTC_WINKITS=%ProgramFiles(x86)%\Windows Kits\10"
set "SOTC_SDKVER="
for /f "delims=" %%v in ('dir /b /ad /o-n "%SOTC_WINKITS%\Include\10.*" 2^>nul') do if not defined SOTC_SDKVER if exist "%SOTC_WINKITS%\Include\%%v\ucrt\crtdbg.h" set "SOTC_SDKVER=%%v"

rem Start from a clean compiler environment so a previous/foreign vcvars run cannot leak in.
set "INCLUDE="
set "LIB="
set "LIBPATH="
set "EXTERNAL_INCLUDE="
set "WindowsSdkDir="
set "WindowsSDKVersion="
set "UCRTVersion="
set "UniversalCRTSdkDir="
set "VCINSTALLDIR="
set "VSINSTALLDIR="
set "VSCMD_VER="
set "VSCMD_ARG_TGT_ARCH="
set "VSCMD_ARG_HOST_ARCH="
set "__VSCMD_PREINIT_PATH="

set "SOTC_VCVARS_LOG=%TEMP%\sotc_vcvars.log"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" %SOTC_SDKVER% >"%SOTC_VCVARS_LOG%" 2>&1
if errorlevel 1 goto vcvarsfail

rem Verify the Windows SDK made it into INCLUDE (vcvars can "succeed" without it).
set "SOTC_UCRT_OK="
for %%d in ("%INCLUDE:;=" "%") do if exist "%%~d\crtdbg.h" set "SOTC_UCRT_OK=1"
if not defined SOTC_UCRT_OK goto nosdk

set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
%*
exit /b %ERRORLEVEL%

:novs
echo Visual Studio 2022 not found. Set VSROOT to your Visual Studio installation directory.
exit /b 1

:vcvarsfail
echo vcvars64.bat failed. Its output:
type "%SOTC_VCVARS_LOG%"
exit /b 1

:nosdk
echo The Windows SDK UCRT headers (crtdbg.h) are not on INCLUDE after vcvars64.bat.
echo   VSROOT=%VSROOT%
echo   Windows SDK picked: "%SOTC_SDKVER%" (from %SOTC_WINKITS%\Include)
echo   INCLUDE=%INCLUDE%
echo vcvars64.bat output:
type "%SOTC_VCVARS_LOG%"
exit /b 1
