<#
.SYNOPSIS
    Install the Windows build toolchain for Satellite: scripts/install-deps.ps1 [-Msvc]

.DESCRIPTION
    Installs exactly the toolchain CI uses, so a local build is the build CI
    runs.

    Default (the windows-ci.yml lane, MSYS2 MINGW64):
      [1] MSYS2 (winget MSYS2.MSYS2)
      [2] pacman: mingw-w64-x86_64-{gcc,cmake,ninja,make,pkgconf,openssl,opus}
          (the exact package list msys2/setup-msys2 installs in windows-ci.yml;
          openssl backs the HTTPS client API, opus is the controller-audio codec)
      [3] user PATH: C:\msys64\mingw64\bin added, C:\msys64\ucrt64\bin removed
          (UCRT64 is a different toolchain than CI's MINGW64; an old version of
          this script installed it and having it first on PATH re-creates the
          drift this script exists to end)

    Shared extras (all lanes):
      [4] clang-format 22.1.4 via pip, the same pin every CI lane uses
      [5] Kitware CMake (presets + ctest driver outside the MSYS2 shell)
      [6] Inno Setup 6 (scripts/build-installer.ps1)
      [7] .NET SDK 10 (dotnet publish of helper\hidmaestro)
      [8] LLVM (clang-tidy) + Cppcheck, the documented code-quality extras.
          Note: LLVM also ships a floating clang-format; the pinned pip
          22.1.4 is the one the format gate trusts.

    -Msvc additionally bootstraps the hardened lane windows-msvc-ci.yml and
    release.yml build (MSVC + vcpkg, CFG + CET + Spectre):
      * VS 2022 Build Tools with the C++ x64 toolset (winget, if no VS found)
      * a vcpkg checkout at %USERPROFILE%\vcpkg (git clone + bootstrap, if
        VCPKG_ROOT is not already set)

    NOT installed (runtime-only): the ViGEmBus driver. Grab it from
    https://github.com/nefarius/ViGEmBus/releases to run the receiver. The
    HIDMaestro driver deploys via the bundled helper, elevated.

    Requires winget (ships with Windows 11; Microsoft Store "App Installer"
    otherwise). Some installers trigger a UAC prompt; approve them when asked.
#>
[CmdletBinding()]
param(
    [switch]$Msvc
)

$ErrorActionPreference = 'Stop'
$msys2Root = 'C:\msys64'
$mingwBin = Join-Path $msys2Root 'mingw64\bin'
$ucrtBin = Join-Path $msys2Root 'ucrt64\bin'
# The exact MINGW64 package set windows-ci.yml installs via msys2/setup-msys2.
$pacmanPackages = @(
    'mingw-w64-x86_64-gcc',
    'mingw-w64-x86_64-cmake',
    'mingw-w64-x86_64-ninja',
    'mingw-w64-x86_64-make',
    'mingw-w64-x86_64-pkgconf',
    'mingw-w64-x86_64-openssl',
    'mingw-w64-x86_64-opus'
)

function Step([string]$Text) { Write-Output ''; Write-Output "=== $Text ===" }

function Invoke-Winget([string]$Id, [string[]]$ExtraArgs = @()) {
    & winget install --id $Id --silent --accept-source-agreements --accept-package-agreements @ExtraArgs
    # winget exits non-zero for "already installed" variants; report, don't die.
    if ($LASTEXITCODE -ne 0) {
        Write-Output "[NOTE] winget install $Id exited $LASTEXITCODE (usually: already installed)."
    }
}

function Add-UserPathEntry([string]$Dir) {
    $p = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $p) { $p = '' }
    if (($p -split ';') -contains $Dir) {
        Write-Output "[OK]  $Dir already on user PATH"
        return
    }
    [Environment]::SetEnvironmentVariable('Path', ($p.TrimEnd(';') + ';' + $Dir).TrimStart(';'), 'User')
    Write-Output "[OK]  Added $Dir to user PATH (open a new terminal to pick it up)"
}

function Remove-UserPathEntry([string]$Dir) {
    $p = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $p) { return }
    $parts = $p -split ';'
    if (-not ($parts -contains $Dir)) { return }
    $rest = ($parts | Where-Object { $_ -and $_ -ne $Dir }) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $rest, 'User')
    Write-Output "[OK]  Removed $Dir from user PATH (different toolchain than CI's MINGW64; the packages stay installed)"
}

Step 'Checking winget'
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Error 'winget not found on PATH. Install "App Installer" from the Microsoft Store, then re-run.'
}
Write-Output '[OK]  winget available'

Step '[1/8] MSYS2 (provides the MINGW64 toolchain CI builds with)'
if (Test-Path (Join-Path $msys2Root 'usr\bin\pacman.exe')) {
    Write-Output "[OK]  MSYS2 already at $msys2Root"
} else {
    Invoke-Winget 'MSYS2.MSYS2'
    if (-not (Test-Path (Join-Path $msys2Root 'usr\bin\pacman.exe'))) {
        Write-Error ("MSYS2 not found at $msys2Root after install. If it lives elsewhere, run from an MSYS2 shell:`n" +
            "    pacman -S --needed $($pacmanPackages -join ' ')`nthen re-run this script.")
    }
}

Step '[2/8] MINGW64 packages (pacman; the exact set windows-ci.yml installs)'
& (Join-Path $msys2Root 'usr\bin\pacman.exe') -S --needed --noconfirm @pacmanPackages
if ($LASTEXITCODE -ne 0) {
    Write-Error 'pacman could not install the MINGW64 toolchain. Open "MSYS2 MINGW64" from the Start menu, run `pacman -Syu`, then re-run this script.'
}

Step '[3/8] PATH: mingw64 in, ucrt64 out'
Add-UserPathEntry $mingwBin
Remove-UserPathEntry $ucrtBin

Step '[4/8] clang-format 22.1.4 (pip; the pin every CI lane uses)'
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Invoke-Winget 'Python.Python.3.12'
}
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python -m pip install --upgrade pip
    & python -m pip install 'clang-format==22.1.4'
    if ($LASTEXITCODE -ne 0) { Write-Error 'pip could not install clang-format==22.1.4' }
} else {
    Write-Output '[WARN] python still not on PATH (fresh install needs a new terminal).'
    Write-Output '       Re-run this script from a new terminal, or run: python -m pip install clang-format==22.1.4'
}

Step '[5/8] CMake (Kitware; drives the presets outside the MSYS2 shell)'
Invoke-Winget 'Kitware.CMake'

Step '[6/8] Inno Setup 6 (scripts/build-installer.ps1)'
Invoke-Winget 'JRSoftware.InnoSetup'
# Inno Setup may install per-user (winget default) or per-machine; the
# per-user install does not add itself to PATH.
$isccDir = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6')
) | Where-Object { Test-Path (Join-Path $_ 'ISCC.exe') } | Select-Object -First 1
if ($isccDir) {
    Add-UserPathEntry $isccDir
} else {
    Write-Output '[WARN] ISCC.exe not found in standard Inno Setup locations (fresh install may need a new terminal).'
}

Step '[7/8] .NET SDK 10 (HIDMaestro helper build)'
Invoke-Winget 'Microsoft.DotNet.SDK.10'

Step '[8/8] LLVM (clang-tidy) + Cppcheck (code-quality extras)'
Invoke-Winget 'LLVM.LLVM'
Invoke-Winget 'Cppcheck.Cppcheck'

if ($Msvc) {
    Step '[MSVC lane] Visual Studio Build Tools (C++ x64 toolset)'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $haveVc = $false
    if (Test-Path $vswhere) {
        $haveVc = [bool](& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion)
    }
    if ($haveVc) {
        Write-Output '[OK]  A Visual Studio with the C++ x64 toolset is already installed'
    } else {
        Invoke-Winget 'Microsoft.VisualStudio.2022.BuildTools' @(
            '--override',
            '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        )
    }

    Step '[MSVC lane] vcpkg (dependency manifest: vcpkg.json)'
    if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'))) {
        Write-Output "[OK]  VCPKG_ROOT already points at $env:VCPKG_ROOT"
    } else {
        $vcpkgRoot = Join-Path $env:USERPROFILE 'vcpkg'
        if (-not (Test-Path (Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'))) {
            if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
                Write-Error 'git is required to clone vcpkg. Install git, or set VCPKG_ROOT to an existing checkout.'
            }
            & git clone https://github.com/microsoft/vcpkg $vcpkgRoot
            if ($LASTEXITCODE -ne 0) { Write-Error 'git clone of vcpkg failed' }
            & (Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
            if ($LASTEXITCODE -ne 0) { Write-Error 'vcpkg bootstrap failed' }
        }
        [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $vcpkgRoot, 'User')
        Write-Output "[OK]  vcpkg at $vcpkgRoot; VCPKG_ROOT set in the user environment (new terminal to pick it up)"
    }
    Write-Output ''
    Write-Output 'MSVC lane ready: scripts/build.ps1 -Msvc (dependencies build from vcpkg.json on first configure).'
}

Write-Output ''
Write-Output '=== Done ==='
Write-Output ''
Write-Output 'Next steps:'
Write-Output '  1. Open a NEW terminal so the PATH changes take effect.'
Write-Output '  2. Verify:  g++ --version   (must be the C:\msys64\mingw64 one)'
Write-Output '  3. Build:   scripts\build.ps1'
Write-Output '  4. Test:    scripts\build.ps1 release test'
Write-Output '  5. CI parity before pushing:  scripts\ci-local.ps1'
Write-Output ''
Write-Output 'Runtime dependency NOT installed (intentionally):'
Write-Output '  ViGEmBus: https://github.com/nefarius/ViGEmBus/releases'
