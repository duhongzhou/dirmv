# ---------------------------------------------------------------------------
# build_installer.ps1 - 构建 dirMover MSI 安装包 (WiX v4/v5)
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
#
# 前置:
#   1. 已构建 Release 版（build.bat 或 cmake --build build --config Release）
#   2. 已安装 WiX 工具（脚本会自动尝试安装 dotnet 全局工具 wix）
# ---------------------------------------------------------------------------
param(
    [string]$Config = "Release",
    [string]$Version = "1.4.4"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot        # 项目根
$installer = $PSScriptRoot
$buildDir = Join-Path $root "build\$Config"
$distDir = Join-Path $installer "dist"

function Info($m) { Write-Host "  $m" -ForegroundColor Gray }
function Step($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

# --- 1. 确认主程序已构建 ---
Step "检查主程序"
$exe = Join-Path $buildDir "dirMover.exe"
$cli = Join-Path $buildDir "dirMover_cli.exe"
if (-not (Test-Path $exe) -or -not (Test-Path $cli)) {
    Write-Host "  未找到 $buildDir 下的 exe，正在构建..." -ForegroundColor Yellow
    Push-Location $root
    cmake -S . -B build -G "Visual Studio 18 2026" -A x64 | Out-Null
    cmake --build build --config $Config | Out-Null
    Pop-Location
}
if (-not (Test-Path $exe)) { Write-Host "构建失败，找不到 $exe" -ForegroundColor Red; exit 1 }
Info "主程序: $exe"

# --- 2. 确认 WiX ---
Step "检查 WiX 工具"
$wix = Get-Command wix -ErrorAction SilentlyContinue
if (-not $wix) {
    Write-Host "  未检测到 wix 命令，尝试安装 dotnet 全局工具..." -ForegroundColor Yellow
    dotnet tool install --global wix
    $env:PATH += ";$env:USERPROFILE\.dotnet\tools"
    $wix = Get-Command wix -ErrorAction SilentlyContinue
}
if (-not $wix) {
    Write-Host "  WiX 不可用。请安装：dotnet tool install --global wix" -ForegroundColor Red
    exit 1
}
Info "wix: $($wix.Source)"

# --- 3. 构建 MSI ---
Step "构建 MSI"
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Push-Location $installer
try {
    wix build Package.wxs -o "$distDir\dirMover-$Version.msi" -arch x64
    if ($LASTEXITCODE -ne 0) { throw "wix build 失败 (exit $LASTEXITCODE)" }
}
finally {
    Pop-Location
}

$msi = Join-Path $distDir "dirMover-$Version.msi"
if (Test-Path $msi) {
    Write-Host "`n  安装包已生成: $msi" -ForegroundColor Green
    Get-Item $msi | Select-Object Name, Length, LastWriteTime | Format-List
}
