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

REM Clean prior output so stale files (e.g. an old single-file exe) aren't packaged.
if exist "%PUBLISH%" rmdir /s /q "%PUBLISH%"

REM NOTE: no PublishSingleFile - WPF's native interop throws DllNotFoundException
REM from a single-file bundle. The installer packages the whole folder anyway.
echo Publishing self-contained build to %PUBLISH% ...
dotnet publish "%REPO%\src\WinSquish.csproj" -c Release -r win-x64 --self-contained -o "%PUBLISH%" "-p:SquishDll=%SquishDll%"
if errorlevel 1 exit /b 1

REM Make sure the native DLL is next to the exe, regardless of csproj resolution.
copy /y "%SquishDll%" "%PUBLISH%\squish.dll" >nul
if not exist "%PUBLISH%\squish.dll" (
    echo ERROR: failed to stage squish.dll into %PUBLISH%.>&2
    exit /b 1
)

REM --- Optional Azure Trusted Signing ------------------------------------
REM Enabled when a signer is configured (see installer\sign.bat): the
REM committed installer\signer.json, or SIGN_ENDPOINT/SIGN_ACCOUNT/
REM SIGN_CERTIFICATE_PROFILE in the environment. Signing authenticates with
REM your Azure credentials (locally: 'az login'; CI: AZURE_* env vars).
REM When enabled we sign our own binaries here, and hand the same signer to
REM Inno so it signs the installer and its embedded uninstaller too.
set "SIGNBAT=%SCRIPT_DIR%sign.bat"
set "DO_SIGN="
if exist "%SCRIPT_DIR%signer.json"  set "DO_SIGN=1"
if defined SIGN_ENDPOINT            set "DO_SIGN=1"
if defined SIGN_ACCOUNT             set "DO_SIGN=1"
if defined SIGN_CERTIFICATE_PROFILE set "DO_SIGN=1"

set "ISCC_SIGN_FLAGS="
if defined DO_SIGN (
    echo Signing application binaries with Azure Trusted Signing ...
    call "%SIGNBAT%" "%PUBLISH%\WinSquish.exe" "%PUBLISH%\WinSquish.dll" "%PUBLISH%\squish.dll"
    if errorlevel 1 exit /b 1
    set "ISCC_SIGN_FLAGS=/DSign "/Swinsquishsign=cmd.exe /c $q$q%SIGNBAT%$q $f$q""
) else (
    echo No signer configured ^(add installer\signer.json or set SIGN_ENDPOINT/SIGN_ACCOUNT/SIGN_CERTIFICATE_PROFILE^); skipping code signing.
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
"%ISCC%" %ISCC_SIGN_FLAGS% "%SCRIPT_DIR%winsquish.iss"
if errorlevel 1 exit /b 1

echo Done. Installer at %REPO%\build\winsquish-setup.exe
endlocal
