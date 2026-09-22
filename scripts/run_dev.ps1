param(
    [string]$Root = (Join-Path $PSScriptRoot "..")
)

$exe = Join-Path $Root "build\NekoBeat.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "未找到 NekoBeat.exe，请先运行 scripts\build.ps1"
}

& $exe
