@echo off
setlocal EnableExtensions

REM make_image.bat
REM Usage: make_image.bat [-t tmp_dir|--tmp-dir tmp_dir|--temp-dir tmp_dir] "C:\images\data.exfat" "C:\payload"

set "IMAGE="
set "SRCDIR="
set "TEMPDIR="

:parse_args
if "%~1"=="" goto :parse_done
if /i "%~1"=="-t" goto :take_temp
if /i "%~1"=="--tmp-dir" goto :take_temp
if /i "%~1"=="--temp-dir" goto :take_temp
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="--help" goto :usage
if not defined IMAGE (
  set "IMAGE=%~1"
  shift
  goto :parse_args
)
if not defined SRCDIR (
  set "SRCDIR=%~1"
  shift
  goto :parse_args
)
echo [ERROR] Too many arguments.
goto :usage

:take_temp
if "%~2"=="" (
  echo [ERROR] Missing value for %~1
  goto :usage
)
set "TEMPDIR=%~2"
shift
shift
goto :parse_args

:parse_done
if not defined IMAGE goto :usage
if not defined SRCDIR goto :usage

REM Script is expected to be in the same directory as this BAT
set "SCRIPT=%~dp0New-OsfExfatImage.ps1"

if not exist "%SCRIPT%" (
  echo [ERROR] PowerShell script not found: "%SCRIPT%"
  echo Put New-OsfExfatImage.ps1 next to this .bat file.
  exit /b 2
)

if not exist "%SRCDIR%" (
  echo [ERROR] Source directory not found: "%SRCDIR%"
  exit /b 3
)

if not exist "%SRCDIR%\eboot.bin" (
  echo [ERROR] eboot.bin not found in source directory: "%SRCDIR%"
  exit /b 4
)

if defined TEMPDIR (
  if not exist "%TEMPDIR%\" (
    echo [ERROR] Temp directory not found: "%TEMPDIR%"
    exit /b 5
  )
)

REM Run elevated? This BAT does not auto-elevate.
REM Right-click -> Run as administrator, or start cmd as admin.

if defined TEMPDIR (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -ImagePath "%IMAGE%" -SourceDir "%SRCDIR%" -TempDir "%TEMPDIR%" -ForceOverwrite
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -ImagePath "%IMAGE%" -SourceDir "%SRCDIR%" -ForceOverwrite
)

set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo [ERROR] Failed with exit code %RC%.
  exit /b %RC%
)

echo [OK] Done: "%IMAGE%"
exit /b 0

:usage
echo Usage:
echo   %~nx0 [-t tmp_dir ^| --tmp-dir tmp_dir ^| --temp-dir tmp_dir] "C:\path\to\image.img" "C:\path\to\folder"
echo.
echo Examples:
echo   %~nx0 "D:\out\game.exfat" "C:\payload"
echo   %~nx0 -t D:\temp "D:\out\game.exfat" "C:\payload"
echo   %~nx0 "D:\out\game.exfat" "C:\payload" --temp-dir D:\temp
echo.
echo Notes:
echo   - Run this BAT as Administrator.
echo   - Image will be auto-sized.
exit /b 1
