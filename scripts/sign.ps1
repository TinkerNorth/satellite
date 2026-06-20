<#
.SYNOPSIS
    Authenticode-sign Satellite binaries (satellite.exe + SatelliteSetup.exe).

.DESCRIPTION
    Canonical signing recipe for Satellite releases. Reads cert
    selection from environment variables so the same script works for:

      * Local hardware tokens (USB HSM / YubiKey FIPS) via SHA-1 thumbprint
      * Cloud signing (AzureSignTool, DigiCert KeyLocker, SignPath) via
        the CLOUD_SIGN_TOOL env var
      * CI runners with a PKCS#12 cert blob

    Environment variables (any ONE of the cert-selection sets):

      Local cert in user/machine store:
        SATELLITE_SIGN_THUMBPRINT   SHA-1 cert thumbprint (40 hex chars)

      PKCS#12 file:
        SATELLITE_SIGN_PFX          path to .pfx
        SATELLITE_SIGN_PFX_PASSWORD password (or empty)

      Cloud signing:
        CLOUD_SIGN_TOOL             full command line, e.g.
                                    "AzureSignTool sign -kvu https://... -kvi ... -kvs ... -kvc ... -tr http://timestamp.digicert.com -td sha256"

    All modes append SHA-256 file digest + RFC-3161 timestamp. SHA-1
    is not added: Microsoft retired SHA-1 Authenticode in 2016 and
    Windows 10+ ignores it.

    Idempotent: signing an already-signed file just re-signs it.

.PARAMETER Files
    One or more paths to sign. Default: satellite.exe and dist\SatelliteSetup.exe
    (skipped if absent).

.PARAMETER TimestampUrl
    RFC-3161 timestamp server. Default: DigiCert. Falls back to Sectigo
    if the primary fails (mid-sign timeouts have historically been an
    issue with single-vendor timestamping).

.EXAMPLE
    $env:SATELLITE_SIGN_THUMBPRINT = '1234567890ABCDEF...'
    pwsh scripts/sign.ps1

.EXAMPLE
    $env:CLOUD_SIGN_TOOL = 'AzureSignTool sign -kvu ... -tr http://timestamp.digicert.com -td sha256'
    pwsh scripts/sign.ps1 -Files dist\SatelliteSetup.exe
#>
[CmdletBinding()]
param(
    [string[]]$Files = @('satellite.exe', 'dist\SatelliteSetup.exe'),
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string]$TimestampUrlFallback = 'http://timestamp.sectigo.com'
)

$ErrorActionPreference = 'Stop'

function Resolve-SignTool {
    # Prefer signtool.exe from the latest installed Windows SDK. Fall
    # back to PATH (winget installs put it on PATH).
    $candidates = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\bin\x64\signtool.exe",
        "${env:ProgramFiles}\Windows Kits\10\bin\x64\signtool.exe"
    )
    # Globbed SDK version directories: pick newest by name.
    $globbed = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*" `
        -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' }
    foreach ($p in @($globbed) + $candidates) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "signtool.exe not found. Install the Windows 10/11 SDK or add signtool to PATH."
}

function Invoke-SignOne {
    param(
        [string]$File,
        [string]$Timestamp
    )

    if ($env:CLOUD_SIGN_TOOL) {
        # Cloud signing path. The provider's CLI accepts a file at the
        # end of its command line. We append timestamping flags only
        # if the cloud command doesn't already specify a -tr.
        $cmd = $env:CLOUD_SIGN_TOOL
        if ($cmd -notmatch '-tr\s') { $cmd += " -tr $Timestamp -td sha256" }
        $full = "$cmd `"$File`""
        Write-Host "  cloud-sign: $full"
        Invoke-Expression $full
        if ($LASTEXITCODE -ne 0) { throw "cloud sign failed for $File (exit $LASTEXITCODE)" }
        return
    }

    $signtool = Resolve-SignTool
    $args = @('sign', '/fd', 'SHA256', '/tr', $Timestamp, '/td', 'SHA256')

    if ($env:SATELLITE_SIGN_THUMBPRINT) {
        $args += @('/sha1', $env:SATELLITE_SIGN_THUMBPRINT)
    } elseif ($env:SATELLITE_SIGN_PFX) {
        if (-not (Test-Path $env:SATELLITE_SIGN_PFX)) {
            throw "SATELLITE_SIGN_PFX=$($env:SATELLITE_SIGN_PFX) does not exist"
        }
        $args += @('/f', $env:SATELLITE_SIGN_PFX)
        if ($env:SATELLITE_SIGN_PFX_PASSWORD) {
            $args += @('/p', $env:SATELLITE_SIGN_PFX_PASSWORD)
        }
    } else {
        throw "No signing credentials in env. Set one of: SATELLITE_SIGN_THUMBPRINT, SATELLITE_SIGN_PFX, or CLOUD_SIGN_TOOL."
    }

    $args += $File
    Write-Host "  signtool: $signtool $($args -join ' ')"
    & $signtool @args
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $File (exit $LASTEXITCODE)" }
}

function Test-AlreadySigned {
    param([string]$File)
    $sig = Get-AuthenticodeSignature -FilePath $File -ErrorAction SilentlyContinue
    return ($sig -and $sig.Status -eq 'Valid')
}

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $signed   = @()
    $skipped  = @()

    foreach ($rel in $Files) {
        $abs = Join-Path $root $rel
        if (-not (Test-Path $abs)) {
            Write-Host "[skip] $rel (not present)"
            $skipped += $rel
            continue
        }

        Write-Host "[sign] $rel"
        try {
            Invoke-SignOne -File $abs -Timestamp $TimestampUrl
        } catch {
            Write-Warning "primary timestamp ($TimestampUrl) failed: $_"
            Write-Host "[sign] retry with fallback timestamp ($TimestampUrlFallback)"
            Invoke-SignOne -File $abs -Timestamp $TimestampUrlFallback
        }

        if (-not (Test-AlreadySigned -File $abs)) {
            throw "$rel did not validate after signing; aborting."
        }
        $signed += $rel
    }

    Write-Host ""
    Write-Host "=== Signing complete ==="
    Write-Host "  signed:  $($signed.Count)"
    Write-Host "  skipped: $($skipped.Count)"
}
finally {
    Pop-Location
}
