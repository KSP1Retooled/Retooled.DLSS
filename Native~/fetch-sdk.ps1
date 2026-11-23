# Downloads NVIDIA's Streamline SDK release and copies the files that the package needs out of it:
#
#   - the headers, into Native~/streamline/include, for building the plugin
#   - the Streamline and DLSS DLLs, into Plugins/x86_64, for running it
#   - their licenses
#
#   fetch-sdk.ps1                  downloads the release zip from GitHub, unless the files are already there
#   fetch-sdk.ps1 -Zip <path>      uses a release zip that was downloaded before
#   fetch-sdk.ps1 -Force           copies the files again even if they are already there
#
# The script checks the zip against the SHA-256 that GitHub lists for the release. It runs on Windows, Linux and macOS.
param(
    [string]$Zip,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# To move to another Streamline release, change both values. GitHub shows the SHA-256 of each file on the release page.
$version = '2.14.16'
$sha256 = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'

$native = $PSScriptRoot
$streamline = Join-Path $native 'streamline'
$include = Join-Path $streamline 'include'
$plugins = [System.IO.Path]::GetFullPath((Join-Path $native '../Plugins/x86_64'))
$stamp = Join-Path $streamline 'version.txt'

# Each file in the zip that the package needs, by its path below the SDK's top folder, and the folder it goes to.
$files = [ordered]@{
    'license.txt'                   = $streamline
    '3rd-party-licenses.md'         = $streamline
    'bin/x64/sl.interposer.dll'     = $plugins
    'bin/x64/sl.common.dll'         = $plugins
    'bin/x64/sl.dlss.dll'           = $plugins
    'bin/x64/nvngx_dlss.dll'        = $plugins
    'bin/x64/nvngx_dlss.license.txt' = $plugins
}

function Test-Fetched {
    if (-not (Test-Path $stamp) -or (Get-Content $stamp -Raw).Trim() -ne $version) { return $false }
    if (-not (Test-Path (Join-Path $include 'sl.h'))) { return $false }
    foreach ($entry in $files.GetEnumerator()) {
        if (-not (Test-Path (Join-Path $entry.Value (Split-Path $entry.Key -Leaf)))) { return $false }
    }
    return $true
}

if (-not $Force -and (Test-Fetched)) {
    Write-Host "Streamline $version is already in place"
    return
}

$download = $null
if (-not $Zip) {
    $download = Join-Path ([System.IO.Path]::GetTempPath()) "streamline-sdk-v$version-$PID.zip"
    $url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$version/streamline-sdk-v$version.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $download
    $Zip = $download
}

try {
    $actual = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        throw "$Zip has SHA-256 $actual, but Streamline $version's release zip has $sha256"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Zip).Path)
    try {
        # Paths are matched below the folder that holds include/sl.h, so the script still works if a release puts the
        # SDK in a top folder.
        $marker = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/') -match '(^|/)include/sl\.h$' } |
            Select-Object -First 1
        if (-not $marker) { throw "$Zip has no include/sl.h" }
        $root = $marker.FullName.Replace('\', '/') -replace 'include/sl\.h$', ''

        $entries = @{}
        $headers = @()
        foreach ($entry in $archive.Entries) {
            $path = $entry.FullName.Replace('\', '/')
            if (-not $path.StartsWith($root)) { continue }
            $relative = $path.Substring($root.Length)
            $entries[$relative] = $entry
            if ($relative -match '^include/[^/]+\.h$') { $headers += $entry }
        }

        if (Test-Path $include) { Remove-Item -Recurse -Force $include }
        New-Item -ItemType Directory -Force $include, $plugins | Out-Null
        foreach ($header in $headers) {
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($header, (Join-Path $include $header.Name), $true)
        }
        foreach ($file in $files.GetEnumerator()) {
            $entry = $entries[$file.Key]
            if (-not $entry) { throw "$Zip has no $($file.Key)" }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $file.Value $entry.Name), $true)
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    if ($download) { Remove-Item -Force -ErrorAction SilentlyContinue $download }
}

Set-Content -Path $stamp -Value $version
Write-Host "Copied Streamline $version's headers to $include and its DLLs to $plugins"
