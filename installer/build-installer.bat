@echo off
REM Publishes a self-contained WinSquish and compiles the Inno Setup installer.
REM Output: build\winsquish-setup.exe
REM
REM Usage:  installer\build-installer.bat  [C:\path\to\squish.dll]
setlocal enabledelayedexpansion

set "SquishDll=%~1"

REM Repo root is the parent of this script's folder.
set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO=%%~fI"
set "PUBLISH=%REPO%\publish"

echo Publishing self-contained build to %PUBLISH% ...
set "PUB_EXTRA="
if not "%SquishDll%"=="" set "PUB_EXTRA=-p:SquishDll=%SquishDll%"
dotnet publish "%REPO%\src\WinSquish.csproj" -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -o "%PUBLISH%" %PUB_EXTRA%
if errorlevel 1 exit /b 1

if not exist "%PUBLISH%\squish.dll" (
    echo ERROR: squish.dll is missing from the publish folder. Pass the path to squish.dll as the first argument, or make sure ..\squish\squish.dll exists.>&2
    exit /b 1
)

REM Locate ISCC (Inno Setup 6).
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"

if not defined ISCC (
    echo ERROR: Inno Setup 6 ^(ISCC.exe^) not found. Install it from https://jrsoftware.org/isdl.php>&2
    exit /b 1
)

echo Compiling installer with %ISCC% ...
"%ISCC%" "%SCRIPT_DIR%winsquish.iss"
if errorlevel 1 exit /b 1

echo Done. Installer at %REPO%\build\winsquish-setup.exe
endlocal
