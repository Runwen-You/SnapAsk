<#
.SYNOPSIS
Creates a self-contained SnapAsk x64 portable stage, ZIP and SHA-256 file.

.DESCRIPTION
Run from an x64 Visual Studio 2022 developer environment after a successful
Release build. The script uses only the selected build, Qt's windeployqt and
the toolchain's app-local redistributable DLL directory; it performs no
download and never reads the user's SnapAsk configuration or credentials.
#>
[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [string]$OutputRoot,
    [string]$QtBinDirectory,
    [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-CMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CacheText,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $escapedName = [Regex]::Escape($Name)
    $match = [Regex]::Match(
        $CacheText,
        "(?m)^${escapedName}:[^=]+=(.*)$")
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups[1].Value.Trim()
}

function Assert-X64PortableExecutable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    $reader = New-Object System.IO.BinaryReader($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Not a Windows PE executable: $Path"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or $peOffset -gt ($stream.Length - 6)) {
            throw "Invalid PE header offset: $Path"
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Invalid PE signature: $Path"
        }
        if ($reader.ReadUInt16() -ne 0x8664) {
            throw "SnapAsk portable packages must contain an x64 executable: $Path"
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path -Path $PSScriptRoot -ChildPath '..'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot 'out\build\m6-release'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot 'out\portable'
}

$resolvedBuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cachePath = Join-Path $resolvedBuildDirectory 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMakeCache.txt was not found in the exact build directory: $resolvedBuildDirectory"
}
$cacheText = [System.IO.File]::ReadAllText($cachePath)

$buildType = Read-CMakeCacheValue -CacheText $cacheText -Name 'CMAKE_BUILD_TYPE'
$configurationTypes = Read-CMakeCacheValue `
    -CacheText $cacheText -Name 'CMAKE_CONFIGURATION_TYPES'
$sourceExecutable = $null
if ($buildType -ceq 'Release') {
    $sourceExecutable = Join-Path $resolvedBuildDirectory 'SnapAsk.exe'
}
elseif (-not [string]::IsNullOrWhiteSpace($configurationTypes) -and
        ($configurationTypes.Split(';') -ccontains 'Release')) {
    $sourceExecutable = Join-Path $resolvedBuildDirectory 'Release\SnapAsk.exe'
}
else {
    throw 'The selected CMake build directory is not configured for Release.'
}

if (-not (Test-Path -LiteralPath $sourceExecutable -PathType Leaf)) {
    throw "The exact Release executable was not found: $sourceExecutable"
}
Assert-X64PortableExecutable -Path $sourceExecutable

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Read-CMakeCacheValue -CacheText $cacheText `
        -Name 'CMAKE_PROJECT_VERSION'
    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = Read-CMakeCacheValue -CacheText $cacheText `
            -Name 'SnapAsk_VERSION'
    }
}
if ([string]::IsNullOrWhiteSpace($Version) -or
    $Version -cnotmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') {
    throw 'A filesystem-safe package Version could not be determined.'
}

if ([string]::IsNullOrWhiteSpace($QtBinDirectory)) {
    $qt6Directory = Read-CMakeCacheValue -CacheText $cacheText -Name 'Qt6_DIR'
    if ([string]::IsNullOrWhiteSpace($qt6Directory)) {
        throw 'QtBinDirectory was not supplied and Qt6_DIR is absent from CMakeCache.txt.'
    }
    $QtBinDirectory = [System.IO.Path]::GetFullPath(
        (Join-Path -Path $qt6Directory -ChildPath '..\..\..\bin'))
}
else {
    $QtBinDirectory = (Resolve-Path -LiteralPath $QtBinDirectory).Path
}
$winDeployQt = Join-Path $QtBinDirectory 'windeployqt.exe'
if (-not (Test-Path -LiteralPath $winDeployQt -PathType Leaf)) {
    throw "windeployqt.exe was not found at the expected path: $winDeployQt"
}

$vcToolsRedistDirectory = $env:VCToolsRedistDir
if ([string]::IsNullOrWhiteSpace($vcToolsRedistDirectory)) {
    throw 'VCToolsRedistDir is unavailable. Run the script from an x64 Visual Studio developer environment.'
}
$appLocalRuntimeSource = Join-Path $vcToolsRedistDirectory `
    'x64\Microsoft.VC143.CRT'
if (-not (Test-Path -LiteralPath $appLocalRuntimeSource -PathType Container)) {
    throw "The x64 app-local MSVC runtime directory was not found: $appLocalRuntimeSource"
}
$appLocalRuntimeSource = (Resolve-Path -LiteralPath $appLocalRuntimeSource).Path
$appLocalRuntimeFiles = Get-ChildItem -LiteralPath $appLocalRuntimeSource `
    -Filter '*.dll' -File
$requiredRuntimeNames = @(
    'msvcp140.dll',
    'vcruntime140.dll',
    'vcruntime140_1.dll'
)
foreach ($runtimeName in $requiredRuntimeNames) {
    if (-not ($appLocalRuntimeFiles.Name -ccontains $runtimeName)) {
        throw "The app-local MSVC runtime is incomplete: $runtimeName is missing."
    }
}

$resolvedOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath $resolvedOutputRoot)) {
    $null = New-Item -ItemType Directory -Path $resolvedOutputRoot
}
$resolvedOutputRoot = (Resolve-Path -LiteralPath $resolvedOutputRoot).Path

$packageBaseName = "SnapAsk-$Version-win-x64"
$stageDirectory = Join-Path $resolvedOutputRoot $packageBaseName
$zipPath = Join-Path $resolvedOutputRoot "$packageBaseName.zip"
$checksumPath = "$zipPath.sha256"

foreach ($outputPath in @($stageDirectory, $zipPath, $checksumPath)) {
    if (Test-Path -LiteralPath $outputPath) {
        throw "Refusing to overwrite an existing package output: $outputPath"
    }
}

$null = New-Item -ItemType Directory -Path $stageDirectory
$stagedExecutable = Join-Path $stageDirectory 'SnapAsk.exe'
Copy-Item -LiteralPath $sourceExecutable -Destination $stagedExecutable

& $winDeployQt `
    --release `
    --no-compiler-runtime `
    --no-translations `
    --no-system-d3d-compiler `
    $stagedExecutable
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE. The partial stage was retained for diagnosis."
}

Assert-X64PortableExecutable -Path $stagedExecutable

foreach ($runtimeFile in $appLocalRuntimeFiles) {
    Copy-Item -LiteralPath $runtimeFile.FullName -Destination $stageDirectory
}
foreach ($runtimeName in $requiredRuntimeNames) {
    if (-not (Test-Path -LiteralPath `
            (Join-Path $stageDirectory $runtimeName) -PathType Leaf)) {
        throw "The app-local MSVC runtime was not staged: $runtimeName"
    }
}

# The stage was created empty and is populated only from the selected Release
# executable and windeployqt. Reject common user-state or diagnostic artifacts
# as a final defense against accidentally shipping configuration or secrets.
$forbiddenExactNames = @(
    'providers.json',
    'consent.ini',
    'settings.ini',
    'snapask.ini',
    'vc_redist.x64.exe'
)
$stagedFiles = Get-ChildItem -LiteralPath $stageDirectory -File -Recurse
foreach ($file in $stagedFiles) {
    if ($forbiddenExactNames -ccontains $file.Name.ToLowerInvariant() -or
        $file.Extension -ieq '.log' -or
        $file.Extension -ieq '.dmp' -or
        $file.Extension -ieq '.pdb') {
        throw "Forbidden user-state or diagnostic artifact entered the package: $($file.FullName)"
    }
}

Compress-Archive -LiteralPath $stageDirectory `
    -DestinationPath $zipPath -CompressionLevel Optimal
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    throw "Portable ZIP was not created: $zipPath"
}

$sha256 = [System.Security.Cryptography.SHA256]::Create()
$zipStream = [System.IO.File]::OpenRead($zipPath)
try {
    $hashBytes = $sha256.ComputeHash($zipStream)
}
finally {
    $zipStream.Dispose()
    $sha256.Dispose()
}
$zipHash = -join ($hashBytes | ForEach-Object { $_.ToString('x2') })
$checksumLine = "{0} *{1}" -f $zipHash,
    [System.IO.Path]::GetFileName($zipPath)
Set-Content -LiteralPath $checksumPath -Value $checksumLine `
    -Encoding Ascii -NoNewline

Write-Host "Release executable: $sourceExecutable"
Write-Host "Portable stage:     $stageDirectory"
Write-Host "Portable ZIP:       $zipPath"
Write-Host "SHA-256:            $zipHash"
