param(
    [string]$Root = (Join-Path $PSScriptRoot ".."),
    [string]$QtDir = "C:\Qt\6.9.3\mingw_64",
    [string]$MinGwDir = "C:\Qt\Tools\mingw1310_64",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path -LiteralPath $Root).Path
$sourceRoot = $Root
$mappedDrive = $null

# MinGW Makefiles mis-encode non-ASCII source paths. Map the parent folder to a
# temporary ASCII drive so the source stays where the user put it.
if ($Root -match '[^\x00-\x7F]') {
    $parent = Split-Path -Parent $Root

    # Reuse an existing ASCII mapping to the same parent when possible.
    foreach ($letter in @('R','S','T','U','V','W','X','Y','Z')) {
        $drive = "${letter}:"
        # A drive that does not exist must not make Join-Path throw on
        # PowerShell 7, so probe the drive root first.
        if ((Test-Path "$drive\") `
            -and (Test-Path -LiteralPath (Join-Path $drive "NekoBeat\CMakeLists.txt"))) {
            $mappedDrive = $drive
            $sourceRoot = Join-Path $drive (Split-Path -Leaf $Root)
            break
        }
    }

    if (-not $mappedDrive) {
        foreach ($letter in @('R','S','T','U','V','W','X','Y','Z')) {
        $drive = "${letter}:"
        if (-not (Test-Path "$drive\")) {
            subst $drive $parent
            $mappedDrive = $drive
            $sourceRoot = Join-Path $drive (Split-Path -Leaf $Root)
            break
        }
        }
    }
    if (-not $mappedDrive) {
        throw "找不到可用的 ASCII 盘符，无法为 MinGW 构建映射中文路径。"
    }
}

$env:PATH = "$QtDir\bin;$MinGwDir\bin;$env:PATH"
$buildDir = Join-Path $sourceRoot "build"

cmake -S $sourceRoot -B $buildDir -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH="$QtDir" `
    -DMPV_ROOT="$(Join-Path $sourceRoot 'third_party\mpv')" `
    -DNEKOBEAT_BUILD_TESTS=ON

cmake --build $buildDir --config Release -j 8
if ($LASTEXITCODE -ne 0) {
    throw "构建失败，退出码 $LASTEXITCODE"
}

if (-not $SkipTests) {
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "测试失败，退出码 $LASTEXITCODE"
    }
}

$releaseDir = $buildDir
if (Test-Path (Join-Path $buildDir "release\NekoBeat.exe")) {
    $releaseDir = Join-Path $buildDir "release"
} elseif (Test-Path (Join-Path $buildDir "bin\NekoBeat.exe")) {
    $releaseDir = Join-Path $buildDir "bin"
}
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

Copy-Item -LiteralPath (Join-Path $sourceRoot "third_party\mpv\libmpv-2.dll") `
    -Destination $releaseDir -Force

& "$QtDir\bin\windeployqt.exe" --release --no-translations `
    (Join-Path $releaseDir "NekoBeat.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt 失败，退出码 $LASTEXITCODE"
}

Write-Host "构建完成: $releaseDir"

if ($mappedDrive) {
    Write-Host "源码通过临时盘符 $mappedDrive 构建，真实路径仍为: $Root"
}
