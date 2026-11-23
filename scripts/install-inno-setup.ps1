<#
.SYNOPSIS
    Install the pinned Inno Setup 6 and print the path of its ISCC.exe.

.DESCRIPTION
    The one place the Inno Setup version lives. windows-msvc-ci.yml and
    release.yml compile the installer with the ISCC this script installs, and
    scripts/install-deps.ps1 calls it for a local machine, so the installer a
    pull request compiles is compiled by the same Inno Setup as the one a tag
    ships. The download is the release asset on jrsoftware/issrc, checked
    against the SHA-256 GitHub publishes for it before it runs.

    Inno Setup 7 is a separate decision (installer.iss has not been moved to
    it); bump within 6.x here.

.PARAMETER Destination
    Install directory. Defaults to RUNNER_TEMP\inno-setup-<version> in CI and
    %LOCALAPPDATA%\Programs\Inno Setup <version> elsewhere.
#>
[CmdletBinding()]
param(
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'

$version = '6.7.4'
$sha256 = 'a549929319f9120716677c685535ca4671e4b937b5fdc87e2585f32d8eb30e8c'
$tag = 'is-' + ($version -replace '\.', '_')
$url = "https://github.com/jrsoftware/issrc/releases/download/$tag/innosetup-$version.exe"

if (-not $Destination) {
    if ($env:RUNNER_TEMP) { $Destination = Join-Path $env:RUNNER_TEMP "inno-setup-$version" }
    else { $Destination = Join-Path $env:LOCALAPPDATA "Programs\Inno Setup $version" }
}
$iscc = Join-Path $Destination 'ISCC.exe'

if (-not (Test-Path $iscc)) {
    $tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
    $setup = Join-Path $tempRoot "innosetup-$version.exe"
    Invoke-WebRequest -Uri $url -OutFile $setup -UseBasicParsing
    $got = (Get-FileHash -Algorithm SHA256 $setup).Hash.ToLowerInvariant()
    if ($got -ne $sha256) { throw "innosetup-$version.exe has SHA-256 $got, expected $sha256" }
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/CURRENTUSER', "/DIR=`"$Destination`"")
    $process = Start-Process -FilePath $setup -ArgumentList $arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "innosetup-$version.exe exited $($process.ExitCode)" }
    if (-not (Test-Path $iscc)) { throw "Inno Setup $version installed no ISCC.exe at $iscc" }
}

# ISCC.exe's version resource: its file version reads 6.7.4.0 and its
# product version a display string, so the pin is matched in either.
$info = (Get-Item $iscc).VersionInfo
$stamped = "$($info.FileVersion) | $($info.ProductVersion)"
if ($stamped -notmatch ('(^|\D)' + [regex]::Escape($version) + '(\D|$)')) {
    throw "ISCC.exe at $iscc carries version '$stamped', expected Inno Setup $version"
}
Write-Output $iscc
