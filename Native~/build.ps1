# Builds Retooled.DLSS.Native.dll and the test harness. Needs CMake, Ninja, LLVM's clang-cl, the Windows SDK and the
# Vulkan headers, from the Vulkan SDK or from a Vulkan-Headers checkout passed as -VulkanInclude. It runs
# fetch-sdk.ps1 first, which downloads NVIDIA's Streamline SDK if its files aren't in place yet.
#
#   build.ps1            builds the plugin and copies it into Plugins/x86_64
#   build.ps1 -Test      also runs the harness on Direct3D 11 and Direct3D 12, which needs an NVIDIA RTX GPU
#
# A Unity editor that has loaded the plugin keeps the DLL open, so close it before building.
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$VulkanInclude,
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

$vulkan = if ($VulkanInclude) { "-DVulkan_INCLUDE_DIR=$VulkanInclude" }
cmake -S $native -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    -DCMAKE_CXX_COMPILER=clang-cl `
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON `
    $vulkan
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build $build
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

if ($Test) {
    # The harness loads Streamline from its own folder, so it runs from a folder that holds the SDK's DLLs. It runs
    # twice on each API: with nvngx_dlss.dll next to Streamline, as in a player, and with it in a folder of its own,
    # which the plugin is told about, as in an editor where the package can't hold it.
    $run = Join-Path $build 'harness'
    $runtime = Join-Path $build 'harness-runtime'
    if (Test-Path $run) { Remove-Item -Recurse -Force $run }
    New-Item -ItemType Directory -Force $run, $runtime | Out-Null
    Copy-Item (Join-Path $build 'DlssHarness.exe') $run -Force
    foreach ($name in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll') {
        Copy-Item (Join-Path $plugins $name) $run -Force
    }
    Copy-Item (Join-Path $plugins 'nvngx_dlss.dll') $runtime -Force
    foreach ($api in 'd3d11', 'd3d12') {
        & (Join-Path $run 'DlssHarness.exe') $api 16 $runtime
        if ($LASTEXITCODE -ne 0) { throw "The harness failed on $api with nvngx_dlss.dll in its own folder" }
    }
    Copy-Item (Join-Path $plugins 'nvngx_dlss.dll') $run -Force
    foreach ($api in 'd3d11', 'd3d12') {
        & (Join-Path $run 'DlssHarness.exe') $api
        if ($LASTEXITCODE -ne 0) { throw "The harness failed on $api with nvngx_dlss.dll next to Streamline" }
    }
}

Copy-Item (Join-Path $build 'Retooled.DLSS.Native.dll') $plugins -Force
Write-Host "Copied Retooled.DLSS.Native.dll to $(Resolve-Path $plugins)"
