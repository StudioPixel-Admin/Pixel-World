param([switch]$Run, [switch]$Test, [switch]$CoreOnly)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
# Import the compiler environment into this process without modifying the user's machine.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++ first.' }
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio C++ build tools were not found.' }
    $devModule = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    Import-Module $devModule
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$preset = if ($CoreOnly) { 'core' } else { 'release' }
Push-Location $projectRoot
try {
    cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    cmake --build --preset $preset --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
    if ($Test) { ctest --preset $preset; if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' } }
    if ($Run -and -not $CoreOnly) { & (Join-Path $projectRoot 'build\bin\PixelWorld.exe') }
} finally { Pop-Location }
