<#
.SYNOPSIS
    Generate a CycloneDX SBOM for the Satellite release.

.DESCRIPTION
    Emits dist\satellite-sbom.cdx.json describing the runtime supply
    chain of the shipped binary: vendored libsodium, ViGEmBus, OpenSSL
    (statically linked from MSYS2), cpp-httplib, etc.

    Prefers `syft` if present (it can scan the .exe and emit a
    deterministic SBOM). Falls back to a hand-rolled minimal CycloneDX
    document built from /VERSION + redist/SHA256SUMS + lib/* paths,
    enough to ship an SBOM without external tooling.

    Tools to reach for to upgrade later:
      * syft           (CycloneDX & SPDX, scans binaries)
      * cyclonedx-cli  (validation, merging, signing)
      * grype          (vuln scan against the SBOM)
#>
[CmdletBinding()]
param(
    [string]$OutFile = 'dist\satellite-sbom.cdx.json'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
    New-Item -ItemType Directory -Path (Split-Path $OutFile) -Force -ErrorAction SilentlyContinue | Out-Null

    $syft = Get-Command syft -ErrorAction SilentlyContinue
    if ($syft -and (Test-Path 'satellite.exe')) {
        Write-Host "[sbom] using syft"
        & syft 'satellite.exe' -o "cyclonedx-json=$OutFile" --quiet
        if ($LASTEXITCODE -ne 0) { throw "syft failed (exit $LASTEXITCODE)" }
        Write-Host "[sbom] wrote $OutFile (syft)"
        return
    }

    Write-Host "[sbom] syft not found; emitting hand-rolled CycloneDX minimum"

    # Hand-roll a minimal CycloneDX 1.5 document. Components come from
    # the vendored prereq files we know about. Add new dependencies
    # here as they get vendored.
    $components = @(
        @{
            type      = 'application'
            'bom-ref' = "pkg:github/tinkernorth/satellite@$version"
            name      = 'satellite'
            version   = $version
            publisher = 'TinkerNorth'
            licenses  = @(@{ license = @{ id = 'LGPL-3.0-or-later' } })
            purl      = "pkg:github/tinkernorth/satellite@$version"
        },
        @{
            type      = 'library'
            'bom-ref' = 'pkg:generic/libsodium@vendored'
            name      = 'libsodium'
            version   = 'vendored (see lib/libsodium/)'
            licenses  = @(@{ license = @{ id = 'ISC' } })
            description = 'Cryptographic primitives used for key agreement and authenticated encryption.'
        },
        @{
            type      = 'library'
            'bom-ref' = 'pkg:generic/cpp-httplib@vendored'
            name      = 'cpp-httplib'
            version   = 'vendored (lib/httplib.h)'
            licenses  = @(@{ license = @{ id = 'MIT' } })
            description = 'Header-only HTTP/1.1 server + client (OpenSSL-backed TLS).'
        },
        @{
            type      = 'library'
            'bom-ref' = 'pkg:generic/openssl@msys2-static'
            name      = 'OpenSSL'
            version   = 'MSYS2 ucrt64 static (see install-dependencies.bat)'
            licenses  = @(@{ license = @{ id = 'Apache-2.0' } })
            description = 'TLS for the client API server, statically linked.'
        },
        @{
            type      = 'library'
            'bom-ref' = 'pkg:generic/vigembus@1.22.0'
            name      = 'ViGEmBus'
            version   = '1.22.0'
            publisher = 'Nefarius Software Solutions e.U.'
            licenses  = @(@{ license = @{ id = 'BSD-3-Clause' } })
            description = 'Bundled redist prerequisite; installed once at machine scope. Drives virtual gamepads + controller motion.'
        }
    )

    $bom = [ordered]@{
        bomFormat    = 'CycloneDX'
        specVersion  = '1.5'
        version      = 1
        serialNumber = "urn:uuid:$([guid]::NewGuid())"
        metadata     = [ordered]@{
            timestamp = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')
            tools     = @(@{ vendor = 'TinkerNorth'; name = 'scripts/generate-sbom.ps1'; version = '1.0' })
            component = $components[0]
        }
        components   = $components | Select-Object -Skip 1
    }

    $bom | ConvertTo-Json -Depth 12 | Set-Content -Path $OutFile -Encoding utf8
    Write-Host "[sbom] wrote $OutFile (hand-rolled)"
}
finally {
    Pop-Location
}
