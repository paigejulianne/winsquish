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

REM Resolve the native squish.dll: explicit arg, else the sibling ..\squish checkout.
if "%SquishDll%"=="" for %%I in ("%REPO%\..\squish\squish.dll") do set "SquishDll=%%~fI"
if not exist "%SquishDll%" (
    echo ERROR: squish.dll not found at "%SquishDll%". Pass the path to squish.dll as the first argument, or make sure ..\squish\squish.dll exists.>&2
    exit /b 1
)

echo Publishing self-contained build to %PUBLISH% ...
dotnet publish "%REPO%\src\WinSquish.csproj" -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -o "%PUBLISH%" "-p:SquishDll=%SquishDll%"
if errorlevel 1 exit /b 1

REM Single-file publish doesn't reliably stage loose native content, so copy it in.
copy /y "%SquishDll%" "%PUBLISH%\squish.dll" >nul
if not exist "%PUBLISH%\squish.dll" (
    echo ERROR: failed to stage squish.dll into %PUBLISH%.>&2
    exit /b 1
)

REM Locate ISCC (Inno Setup 7 or 6).
set "ISCC="
for %%D in (
    "%ProgramFiles%\Inno Setup 7"
    "%ProgramFiles(x86)%\Inno Setup 7"
    "%ProgramFiles%\Inno Setup 6"
    "%ProgramFiles(x86)%\Inno Setup 6"
) do if not defined ISCC if exist "%%~D\ISCC.exe" set "ISCC=%%~D\ISCC.exe"

if not defined ISCC (
    echo ERROR: Inno Setup ^(ISCC.exe^) not found. Install it from https://jrsoftware.org/isdl.php>&2
    exit /b 1
)

echo Compiling installer with %ISCC% ...
"%ISCC%" "%SCRIPT_DIR%winsquish.iss"
if errorlevel 1 exit /b 1

echo Done. Installer at %REPO%\build\winsquish-setup.exe
endlocal
