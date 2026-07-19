@echo off
setlocal enabledelayedexpansion
REM ===========================================================================
REM  Azure Trusted Signing helper for WinSquish (batch port of the old sign.ps1).
REM
REM  Signs one or more files with the 'sign' dotnet global tool
REM  (https://github.com/dotnet/sign) using an Azure Trusted Signing account.
REM  Reused for both the application binaries (from build-installer.bat) and the
REM  installer + embedded uninstaller (from Inno Setup's SignTool directive).
REM
REM  Signer details are read at build time from signer.json (next to this
REM  script). No secret lives in the repo: the JSON holds only the account
REM  endpoint / name / certificate profile, and signing authenticates with your
REM  Azure credentials --
REM      locally:  run 'az login' once (AzureCliCredential)
REM      CI:       set AZURE_TENANT_ID / AZURE_CLIENT_ID / AZURE_CLIENT_SECRET,
REM                or use a managed / workload identity
REM  via the tool's DefaultAzureCredential chain.
REM
REM  signer.json fields (all overridable by the matching environment variable):
REM      endpoint            SIGN_ENDPOINT             Trusted Signing account URI
REM      account             SIGN_ACCOUNT              account name
REM      certificateProfile  SIGN_CERTIFICATE_PROFILE  certificate profile name
REM      timestampUrl        SIGN_TIMESTAMP_URL        RFC-3161 timestamp server
REM      azureCredentialType SIGN_AZURE_CREDENTIAL_TYPE  azure-cli | azure-powershell
REM                                                     | managed-identity | workload-identity
REM  Optional:
REM      SIGN_TOOL           explicit path to the 'sign' executable (else PATH,
REM                          then %%USERPROFILE%%\.dotnet\tools\sign.exe)
REM
REM  Usage:  sign.bat <file> [<file> ...]
REM ===========================================================================

if "%~1"=="" (
    echo sign: no files to sign. Usage: sign.bat ^<file^> [^<file^> ...]>&2
    exit /b 1
)

set "SCRIPT_DIR=%~dp0"
set "CFG=%SCRIPT_DIR%signer.json"

REM --- locate the 'sign' global tool ----------------------------------------
set "SIGNTOOL=%SIGN_TOOL%"
if defined SIGNTOOL if not exist "!SIGNTOOL!" set "SIGNTOOL="
if not defined SIGNTOOL (
    REM Search for sign.exe specifically so this never resolves to our own sign.bat.
    for /f "delims=" %%I in ('where sign.exe 2^>nul') do if not defined SIGNTOOL set "SIGNTOOL=%%I"
)
if not defined SIGNTOOL (
    if exist "%USERPROFILE%\.dotnet\tools\sign.exe" set "SIGNTOOL=%USERPROFILE%\.dotnet\tools\sign.exe"
)
if not defined SIGNTOOL (
    echo sign: the 'sign' tool was not found. Install it with 'dotnet tool install --global sign --prerelease', or set the SIGN_TOOL environment variable to its full path.>&2
    exit /b 1
)

REM --- signer configuration (signer.json, overridden by SIGN_* env vars) -----
call :pick ENDPOINT  SIGN_ENDPOINT              endpoint
call :pick ACCOUNT   SIGN_ACCOUNT               account
call :pick PROFILE   SIGN_CERTIFICATE_PROFILE   certificateProfile
call :pick TIMESTAMP SIGN_TIMESTAMP_URL         timestampUrl
call :pick CREDTYPE  SIGN_AZURE_CREDENTIAL_TYPE azureCredentialType

set "MISSING="
if not defined ENDPOINT set "MISSING=!MISSING! endpoint (SIGN_ENDPOINT)"
if not defined ACCOUNT  set "MISSING=!MISSING! account (SIGN_ACCOUNT)"
if not defined PROFILE  set "MISSING=!MISSING! certificateProfile (SIGN_CERTIFICATE_PROFILE)"
if defined MISSING (
    echo sign: incomplete signer configuration -- missing:!MISSING!. Set these in installer\signer.json or via the environment.>&2
    exit /b 1
)

REM --- build the argument list ----------------------------------------------
set "SIGNARGS=code artifact-signing --artifact-signing-endpoint "%ENDPOINT%" --artifact-signing-account "%ACCOUNT%" --artifact-signing-certificate-profile "%PROFILE%" --file-digest sha256"
if defined TIMESTAMP set "SIGNARGS=!SIGNARGS! --timestamp-url "%TIMESTAMP%""
if defined CREDTYPE  set "SIGNARGS=!SIGNARGS! --azure-credential-type "%CREDTYPE%""

REM --- sign each file --------------------------------------------------------
:signloop
if "%~1"=="" goto :signdone
if not exist "%~1" (
    echo sign: file to sign not found: %~1>&2
    exit /b 1
)
echo Signing %~1
"%SIGNTOOL%" !SIGNARGS! "%~1"
if errorlevel 1 (
    echo sign: 'sign' failed for '%~1'.>&2
    exit /b 1
)
shift
goto :signloop
:signdone
endlocal
exit /b 0

REM ===========================================================================
REM  Subroutines
REM ===========================================================================

REM :pick <outVar> <envVar> <jsonKey>
REM   Sets <outVar> to the environment variable's value if set, else to the
REM   value read from signer.json for <jsonKey> (empty if neither is present).
:pick
call set "_pv=%%%~2%%"
if defined _pv (
    set "%~1=!_pv!"
    goto :eof
)
call :cfgval "%~3" %~1
goto :eof

REM :cfgval <jsonKey> <outVar>
REM   Reads a flat "key": "value" pair from signer.json. Values never contain a
REM   double quote, so we strip all quotes, the trailing comma, and surrounding
REM   whitespace after splitting the line on its first colon.
:cfgval
set "%~2="
if not exist "%CFG%" goto :eof
set "_cv="
for /f "usebackq tokens=1,* delims=:" %%A in (`findstr /r /c:"\"%~1\"[ ]*:" "%CFG%"`) do if not defined _cv set "_cv=%%B"
if not defined _cv goto :eof
set "_cv=!_cv:"=!"
:cfgval_ltrim
if defined _cv if "!_cv:~0,1!"==" " set "_cv=!_cv:~1!" & goto :cfgval_ltrim
if defined _cv if "!_cv:~-1!"=="," set "_cv=!_cv:~0,-1!"
:cfgval_rtrim
if defined _cv if "!_cv:~-1!"==" " set "_cv=!_cv:~0,-1!" & goto :cfgval_rtrim
set "%~2=!_cv!"
goto :eof
