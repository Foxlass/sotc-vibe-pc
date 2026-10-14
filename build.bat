@echo off
setlocal
set "HERE=%~dp0"
set "TARGET=%1"
if "%TARGET%"=="" set "TARGET=sotc"

rem Re-run once at below-normal CPU priority: ninja and every compiler inherit it, so the desktop
rem stays responsive while all cores are busy. SOTC_BUILD_PRIORITY=normal skips this.
if defined SOTC_BUILD_LOWERED goto build
if /i "%SOTC_BUILD_PRIORITY%"=="normal" goto build
set "SOTC_BUILD_LOWERED=1"
start "" /belownormal /b /wait cmd /c ""%~f0" %*"
exit /b %ERRORLEVEL%

:build
call "%HERE%Tools\win\vsenv.cmd" cmake --build "%HERE%build\port" --target %TARGET%
exit /b %ERRORLEVEL%
