param(
    [string]$Root = (Join-Path $PSScriptRoot ".."),
    # Defaults to the version in CMakeLists.txt so a forgotten -Version cannot
    # produce a package labelled with an older release (0.6.2 was the old default
    # and shipped a mislabelled zip).
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $cmakeLists = Join-Path $Root "CMakeLists.txt"
    $match = Select-String -Path $cmakeLists -Pattern '^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' |
        Select-Object -First 1
    if (-not $match) {
        throw "无法从 CMakeLists.txt 解析版本号，请显式传入 -Version。"
    }
    $Version = $match.Matches[0].Groups[1].Value
}

$buildDir = Join-Path $Root "build"
$releaseDir = $buildDir
if (Test-Path (Join-Path $buildDir "release\NekoBeat.exe")) {
    $releaseDir = Join-Path $buildDir "release"
} elseif (Test-Path (Join-Path $buildDir "bin\NekoBeat.exe")) {
    $releaseDir = Join-Path $buildDir "bin"
}

if (-not (Test-Path (Join-Path $releaseDir "NekoBeat.exe"))) {
    throw "未找到构建产物，请先运行 scripts\build.ps1"
}

$packageDir = Join-Path $Root "dist\NekoBeat-$Version-Portable"
if (Test-Path $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

Copy-Item -LiteralPath (Join-Path $releaseDir "NekoBeat.exe") -Destination $packageDir -Force
Get-ChildItem -LiteralPath $releaseDir -Filter "*.dll" -File |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $packageDir -Force }

$pluginDirs = @(
    "generic",
    "iconengines",
    "imageformats",
    "networkinformation",
    "platforms",
    "styles",
    "tls"
)
foreach ($dir in $pluginDirs) {
    $source = Join-Path $releaseDir $dir
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $packageDir -Recurse -Force
    }
}

# The GPL and the LGPL both require their texts to travel with the binaries, and the
# Qt LGPL additionally wants the third-party notice to reach the end user.
Copy-Item -LiteralPath (Join-Path $Root "LICENSE") `
    -Destination (Join-Path $packageDir "LICENSE.txt") -Force
Copy-Item -LiteralPath (Join-Path $Root "THIRD_PARTY_NOTICES.md") `
    -Destination (Join-Path $packageDir "THIRD_PARTY_NOTICES.txt") -Force

$zip = Join-Path $Root "dist\NekoBeat-$Version-Portable.zip"
if (Test-Path $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $zip
[System.IO.Directory]::Delete($packageDir, $true)
Write-Host "便携包: $zip"
