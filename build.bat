@echo off
chcp 65001 >nul
setlocal
title dirMover 构建脚本

echo ============================================
echo   dirMover - 目录软链接迁移工具 构建
echo ============================================
echo.

cd /d "%~dp0"

where cmake >nul 2>&1
if errorlevel 1 (
    echo [错误] 未找到 cmake，请先安装 CMake 并加入 PATH。
    pause
    exit /b 1
)

if "%1"=="" (
    set "CFG=Release"
) else (
    set "CFG=%1"
)

echo [1/3] 配置工程 (%CFG%)...
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (
    echo [错误] CMake 配置失败。若 VS 版本不同，请修改本脚本中的生成器名称。
    pause
    exit /b 1
)

echo.
echo [2/3] 编译...
cmake --build build --config %CFG%
if errorlevel 1 (
    echo [错误] 编译失败。
    pause
    exit /b 1
)

echo.
echo [3/3] 完成！输出位于 build\%CFG%\
echo   - dirMover.exe      （图形界面 / 服务主程序）
echo   - dirMover_cli.exe  （命令行工具）
echo.
dir /b build\%CFG%\*.exe
echo.
pause
