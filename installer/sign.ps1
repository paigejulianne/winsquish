<#
    Azure Trusted Signing helper for WinSquish.

    Signs one or more files with the 'sign' dotnet global tool
    (https://github.com/dotnet/sign) using an Azure Trusted Signing account.
    Reused for both the application binaries (from build-installer.bat) and the
    installer + embedded uninstaller (from Inno Setup's SignTool directive).

    Signer details are read at build time from signer.json (next to this
    script). No secret lives in the repo: the JSON holds only the account
    endpoint / name / certificate profile, and signing authenticates with your
    Azure credentials --
        locally:  run 'az login' once (AzureCliCredential)
        CI:       set AZURE_TENANT_ID / AZURE_CLIENT_ID / AZURE_CLIENT_SECRET,
                  or use a managed / workload identity
    via the tool's DefaultAzureCredential chain.

    signer.json fields (all overridable by the matching environment variable):
        endpoint            SIGN_ENDPOINT             Trusted Signing account URI
        account             SIGN_ACCOUNT              account name
        certificateProfile  SIGN_CERTIFICATE_PROFILE  certificate profile name
        timestampUrl        SIGN_TIMESTAMP_URL        RFC-3161 timestamp server
        azureCredentialType SIGN_AZURE_CREDENTIAL_TYPE  azure-cli | azure-powershell
                                                       | managed-identity | workload-identity
    Optional:
        SIGN_TOOL           explicit path to the 'sign' executable (else PATH,
                            then %USERPROFILE%\.dotnet\tools\sign.exe)

    Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File sign.ps1 <file> [<file> ...]
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]] $Files
)

$ErrorActionPreference = 'Stop'

function Fail($msg) { Write-Error "sign: $msg"; exit 1 }

# --- locate the 'sign' global tool ----------------------------------------
$signTool = $env:SIGN_TOOL
if (-not $signTool) {
    $cmd = Get-Command sign -ErrorAction SilentlyContinue
    if ($cmd) { $signTool = $cmd.Source }
}
if (-not $signTool) {
    $default = Join-Path $env:USERPROFILE '.dotnet\tools\sign.exe'
    if (Test-Path $default) { $signTool = $default }
}
if (-not $signTool -or -not (Test-Path $signTool)) {
    Fail "the 'sign' tool was not found. Install it with 'dotnet tool install --global sign --prerelease', or set the SIGN_TOOL environment variable to its full path."
}

# --- signer configuration (signer.json, overridden by SIGN_* env vars) ----
$cfg = [pscustomobject]@{}
$cfgPath = Join-Path $PSScriptRoot 'signer.json'
if (Test-Path $cfgPath) {
    try { $cfg = Get-Content -Raw -LiteralPath $cfgPath | ConvertFrom-Json }
    catch { Fail "could not parse $cfgPath : $($_.Exception.Message)" }
}
function CfgVal($name) {
    if ($cfg.PSObject.Properties[$name]) { return [string]$cfg.$name }
    return $null
}
function Pick($envVal, $cfgName) {
    if ($envVal) { return $envVal }
    return (CfgVal $cfgName)
}

$endpoint   = Pick $env:SIGN_ENDPOINT             'endpoint'
$account    = Pick $env:SIGN_ACCOUNT              'account'
$profile    = Pick $env:SIGN_CERTIFICATE_PROFILE  'certificateProfile'
$timestamp  = Pick $env:SIGN_TIMESTAMP_URL        'timestampUrl'
$credType   = Pick $env:SIGN_AZURE_CREDENTIAL_TYPE 'azureCredentialType'

$missing = @()
if (-not $endpoint) { $missing += 'endpoint (SIGN_ENDPOINT)' }
if (-not $account)  { $missing += 'account (SIGN_ACCOUNT)' }
if (-not $profile)  { $missing += 'certificateProfile (SIGN_CERTIFICATE_PROFILE)' }
if ($missing.Count) {
    Fail "incomplete signer configuration -- missing: $($missing -join ', '). Set these in installer\signer.json or via the environment."
}

# --- sign -----------------------------------------------------------------
$signArgs = @('code', 'artifact-signing',
              '--artifact-signing-endpoint', $endpoint,
              '--artifact-signing-account', $account,
              '--artifact-signing-certificate-profile', $profile,
              '--file-digest', 'sha256')
if ($timestamp) { $signArgs += @('--timestamp-url', $timestamp) }
if ($credType)  { $signArgs += @('--azure-credential-type', $credType) }

foreach ($file in $Files) {
    if (-not (Test-Path $file)) { Fail "file to sign not found: $file" }
    Write-Host "Signing $file"
    & $signTool @signArgs $file
    if ($LASTEXITCODE -ne 0) { Fail "'sign' failed for '$file' (exit $LASTEXITCODE)." }
}
