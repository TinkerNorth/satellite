$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}

$version = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if (-not $version) {
    throw "vswhere found no Visual Studio installation with the C++ x64 toolset"
}

$major = [int]($version.Split('.')[0])
switch ($major) {
    17 { "Visual Studio 17 2022" }
    18 { "Visual Studio 18 2026" }
    default { throw "No CMake generator mapping for Visual Studio major version $major (installationVersion $version)" }
}
