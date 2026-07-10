@echo off
rem Build winsquish.exe with MSVC. Works from a plain command prompt:
rem locates Visual Studio via vswhere if cl.exe is not already on PATH.

setlocal
cd /d "%~dp0"

where cl.exe >nul 2>nul
if %errorlevel%==0 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo error: cl.exe not on PATH and vswhere.exe not found.
    echo Install Visual Studio with the C++ workload, or run this from a
    echo "Developer Command Prompt for VS".
    exit /b 1
)

"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\winsquish_vspath.txt"
set /p VSPATH=<"%TEMP%\winsquish_vspath.txt"
del "%TEMP%\winsquish_vspath.txt" >nul 2>nul
if not defined VSPATH (
    echo error: no Visual Studio installation with C++ tools found.
    exit /b 1
)

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -no_logo
if errorlevel 1 (
    echo error: failed to initialize the Visual Studio environment.
    exit /b 1
)

:build
if not exist build mkdir build

rc /nologo /fo build\winsquish.res src\winsquish.rc
if errorlevel 1 exit /b 1

rem Link against the prebuilt libsquish DLL (squish\squish.lib is its import
rem library; squish\squish.dll ships beside the exe and in the installer). We
rem no longer compile squish.c in -- /DSQUISH_DLL makes squish.h declare the API
rem as __declspec(dllimport).
cl /nologo /O2 /W4 /MT /utf-8 /DUNICODE /D_UNICODE /DSQUISH_DLL /EHsc /Isquish ^
   /Fo:build\ /Fe:build\winsquish.exe ^
   src\winsquish.cpp build\winsquish.res ^
   /link /SUBSYSTEM:WINDOWS squish\squish.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1

rem The exe needs squish.dll at run time; keep a copy in build\ so it runs
rem straight from there (the installer ships the DLL alongside the exe too).
copy /y squish\squish.dll build\squish.dll >nul
if errorlevel 1 exit /b 1

rem --- Authenticode signing via Azure Trusted Signing (optional) -------------
rem Enabled when sign-metadata.json exists. To set up:
rem   1. copy sign-metadata.template.json to sign-metadata.json and fill in
rem      your Trusted Signing account, certificate profile, and endpoint
rem   2. az login   (the dlib authenticates via Azure CLI credentials)
if not exist sign-metadata.json goto :unsigned

set "DLIB=%LOCALAPPDATA%\Microsoft.Trusted.Signing.Client\bin\x64\Azure.CodeSigning.Dlib.dll"
if not exist "%DLIB%" (
    echo error: sign-metadata.json present but the Trusted Signing client is
    echo missing. Expected: %DLIB%
    exit /b 1
)

set "SIGNTOOL=signtool.exe"
where signtool.exe >nul 2>nul
if not errorlevel 1 goto :sign
set "SIGNTOOL="
for /d %%d in ("%ProgramFiles(x86)%\Windows Kits\10\bin\10.*") do if exist "%%d\x64\signtool.exe" set "SIGNTOOL=%%d\x64\signtool.exe"
if not defined SIGNTOOL (
    echo error: signtool.exe not found ^(install a Windows 10/11 SDK^).
    exit /b 1
)

:sign
"%SIGNTOOL%" sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 ^
    /dlib "%DLIB%" /dmdf sign-metadata.json build\winsquish.exe
if errorlevel 1 (
    echo error: signing failed. Are you logged in? Try: az login
    exit /b 1
)

echo.
echo Built and signed build\winsquish.exe
goto :eof

:unsigned
echo.
echo Built build\winsquish.exe ^(unsigned; create sign-metadata.json to sign^)
