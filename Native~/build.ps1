# Builds RetooledDLSS.dll and the test harness. Needs CMake, Ninja, LLVM's clang-cl and the Windows SDK. It runs
# fetch-sdk.ps1 first, which downloads NVIDIA's Streamline SDK if its files aren't in place yet.
#
#   build.ps1            builds the plugin and copies it into Plugins/x86_64
#   build.ps1 -Test      also runs the harness on Direct3D 11 and Direct3D 12, which needs an NVIDIA RTX GPU
#
# A Unity editor that has loaded the plugin keeps the DLL open, so close it before building.
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$native = $PSScriptRoot
$build = Join-Path $native "build/$Configuration"
$plugins = Join-Path $native '../Plugins/x86_64'

& (Join-Path $native 'fetch-sdk.ps1')

# CMake also needs LLVM's resource compiler and manifest tool, so the whole folder goes on PATH.
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    $llvm = 'C:\Program Files\LLVM\bin'
    if (-not (Test-Path (Join-Path $llvm 'clang-cl.exe'))) { throw "clang-cl was not found on PATH or in $llvm" }
    $env:PATH = "$llvm;$env:PATH"
}

cmake -S $native -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

if ($Test) {
    # The harness loads Streamline from its own folder, so it runs from a folder that holds the SDK's DLLs.
    $run = Join-Path $build 'harness'
    New-Item -ItemType Directory -Force $run | Out-Null
    Copy-Item (Join-Path $build 'DlssHarness.exe') $run -Force
    foreach ($name in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll') {
        Copy-Item (Join-Path $plugins $name) $run -Force
    }
    foreach ($api in 'd3d11', 'd3d12') {
        & (Join-Path $run 'DlssHarness.exe') $api
        if ($LASTEXITCODE -ne 0) { throw "The harness failed on $api" }
    }
}

Copy-Item (Join-Path $build 'RetooledDLSS.dll') $plugins -Force
Write-Host "Copied RetooledDLSS.dll to $(Resolve-Path $plugins)"
