param(
    [string]$Root = (Join-Path $PSScriptRoot ".."),
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$version = "20260903"
$fileName = "mpv-dev-x86_64-20260903-git-69e63f425a.7z"
$url = "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/$version/$fileName"
$sha256 = "FAC135C68A35B7639E39D72C0C365104EDBAEBDEA39A0DFDD8C36E8C8E80FAEF"

$thirdParty = Join-Path $Root "third_party"
$mpvRoot = Join-Path $thirdParty "mpv"
$archive = Join-Path $env:TEMP $fileName

New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

if ($Force -or -not (Test-Path $archive)) {
    Write-Host "下载 libmpv: $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive
}

$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash
if ($actualHash -ne $sha256) {
    throw "libmpv 归档 SHA256 不匹配。实际: $actualHash"
}

if ($Force -and (Test-Path $mpvRoot)) {
    Remove-Item -LiteralPath $mpvRoot -Recurse -Force
}

if (-not (Test-Path (Join-Path $mpvRoot "include/mpv/client.h"))) {
    New-Item -ItemType Directory -Force -Path $mpvRoot | Out-Null
    Write-Host "解压 libmpv 到 $mpvRoot"
    tar -xf $archive -C $mpvRoot
}

Write-Host "libmpv 依赖已就绪: $mpvRoot"

