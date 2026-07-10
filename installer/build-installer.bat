@echo off
rem ==========================================================================
rem Build the WinSquish installer (build\winsquish-setup.exe) with Inno Setup.
rem
rem Builds build\winsquish.exe first if it is missing, then compiles
rem installer\winsquish.iss. Requires Inno Setup 6.3 or later: it locates
rem ISCC.exe on PATH or under Program Files. Get it from:
rem   https://jrsoftware.org/isdl.php
rem ==========================================================================

setlocal
cd /d "%~dp0"

rem --- ensure the application binary exists -------------------------------
if not exist "..\build\winsquish.exe" (
    echo winsquish.exe not found -- building it first...
    call "..\build.bat"
    if errorlevel 1 exit /b 1
)

rem --- locate the Inno Setup compiler ------------------------------------
set "ISCC=ISCC.exe"
where ISCC.exe >nul 2>nul
if not errorlevel 1 goto :compile

set "ISCC="
for %%v in (7 6) do (
    if not defined ISCC if exist "%ProgramFiles%\Inno Setup %%v\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup %%v\ISCC.exe"
    if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup %%v\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup %%v\ISCC.exe"
)
if not defined ISCC (
    echo error: ISCC.exe ^(Inno Setup 6.3+ / 7^) not found on PATH or in Program Files.
    echo Install Inno Setup from https://jrsoftware.org/isdl.php
    exit /b 1
)

:compile
"%ISCC%" /Qp "winsquish.iss"
if errorlevel 1 (
    echo error: Inno Setup compilation failed.
    exit /b 1
)

echo.
echo Built installer: build\winsquish-setup.exe
