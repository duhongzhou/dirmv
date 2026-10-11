@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
title 目录软链接迁移/恢复工具

:: ====================== 管理员权限检查 ======================
net session >nul 2>&1
if errorlevel 1 (
    echo 错误：创建符号链接（mklink）需要管理员权限。
    echo 请右键本脚本，选择“以管理员身份运行”。
    pause >nul
    exit /b 1
)

:: ====================== 预设目录 ======================
set "SOURCE_OPT1=%USERPROFILE%"
set "SOURCE_OPT2=%~dp0"
set "SOURCE_OPT3=手动输入路径"
if "!SOURCE_OPT2:~-1!"=="\" set "SOURCE_OPT2=!SOURCE_OPT2:~0,-1!"

:: ===================== 排除列表 =====================
:: 用 | 分隔，避免空格与引号冲突；匹配时逐项对比
set "DEFAULT_EXCLUDE=AppData|Application Data|Contacts|Cookies|Desktop|Documents|Downloads|Favorites|Links|Local Settings|Music|My Documents|NetHood|OneDrive|Pictures|PrintHood|Recent|Saved Games|Searches|SendTo|Templates|Videos"
:: ======================================================

cls
echo.
echo ====================== 功能选择 ======================
echo  1. 迁移目录（创建软链接）
echo  2. 从日志恢复目录
echo ======================================================
echo.
set "CHOICE="
set /p "CHOICE=请选择功能 [1/2]："

if "%CHOICE%"=="1" goto :MIGRATE
if "%CHOICE%"=="2" goto :RESTORE

echo 输入错误！
pause >nul
exit /b


::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:: 1. 迁移模块
::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:MIGRATE
cls
echo.
echo ============== 源目录选择 ==============
echo  1. !SOURCE_OPT1!
echo  2. !SOURCE_OPT2!
echo  3. !SOURCE_OPT3!
echo.
set "SOURCE_BASE="
set /p "CHOICE=请选择源目录 [1-3]："

if "%CHOICE%"=="1" set "SOURCE_BASE=!SOURCE_OPT1!"
if "%CHOICE%"=="2" set "SOURCE_BASE=!SOURCE_OPT2!"
if "%CHOICE%"=="3" set /p "SOURCE_BASE=请输入源根目录："

:: 去尾斜杠
if "!SOURCE_BASE:~-1!"=="\" set "SOURCE_BASE=!SOURCE_BASE:~0,-1!"

:: 禁止根目录
if /i "!SOURCE_BASE:~2!"=="\" goto :cantroot
if "!SOURCE_BASE:~2!"==""      goto :cantroot
goto :c1

:cantroot
echo 错误：不能选择磁盘根目录！
pause >nul
exit /b
:c1

:: 源目录存在？
if not exist "!SOURCE_BASE!\" goto :dne
goto :c2
:dne
echo 目录不存在：!SOURCE_BASE!
pause >nul
exit /b
:c2

:: 目标目录
echo.
echo ============== 目标目录选择 ==============
echo  1. 脚本所在目录 (%~dp0)
echo  2. 手动输入路径
echo.
set "TARGET_BASE="
set /p "TCHOICE=请选择目标目录 [1-2]："
if "%TCHOICE%"=="1" set "TARGET_BASE=%~dp0"
if "%TCHOICE%"=="2" set /p "TARGET_BASE=请输入存放目标根目录："
if "!TARGET_BASE:~-1!"=="\" set "TARGET_BASE=!TARGET_BASE:~0,-1!"

if not exist "!TARGET_BASE!\" goto :tne
goto :c3
:tne
echo 错误：目标目录不存在，请先创建
pause >nul
exit /b
:c3

:: 防止源/目标相互包含（避免递归自包含）
if /i "!SOURCE_BASE!"=="!TARGET_BASE!" (
    echo 错误：源目录与目标目录不能相同！
    pause >nul
    exit /b 1
)
:: 检查 TARGET 是否以 SOURCE\ 开头（目标在源内部）
set "SRC_PFX=!SOURCE_BASE!\"
set "TGT_PFX=!TARGET_BASE!\"
call :IS_PREFIX "!SRC_PFX!" "!TARGET_BASE!"
if "!_PFX!"=="1" (
    echo 错误：目标目录位于源目录内部，会导致递归自包含！
    pause >nul
    exit /b 1
)
call :IS_PREFIX "!TGT_PFX!" "!SOURCE_BASE!"
if "!_PFX!"=="1" (
    echo 错误：源目录位于目标目录内部！
    pause >nul
    exit /b 1
)
goto :c3ok

:IS_PREFIX
:: %1 = 前缀（含尾斜杠），%2 = 待检查路径；若 %2 以 %1 开头则 _PFX=1
set "_PFX=0"
set "_P1=%~1"
set "_P2=%~2"
set "_LEN=0"
set "_TMP=!_P1!"
:IP_LENLOOP
if defined _TMP (
    set "_TMP=!_TMP:~1!"
    set /a _LEN+=1
    goto :IP_LENLOOP
)
set "_HEAD=!_P2:~0,%_LEN%!"
if /i "!_HEAD!"=="!_P1!" set "_PFX=1"
exit /b

:c3ok

:: 排除设置
echo.
echo 默认不迁移：!DEFAULT_EXCLUDE!
set "USER_EXCLUDE="
set /p "USER_EXCLUDE=额外排除（用 | 分隔，回车无）："
if defined USER_EXCLUDE (
    set "EXCLUDE_LIST=!DEFAULT_EXCLUDE!|!USER_EXCLUDE!"
) else (
    set "EXCLUDE_LIST=!DEFAULT_EXCLUDE!"
)
set "LOG_FILE=!TARGET_BASE!\link_restore_log.txt"

:: 确认
echo.
echo 即将迁移：!SOURCE_BASE!
echo 目标目录：!TARGET_BASE!
echo 日志文件：!LOG_FILE!
pause >nul

:: 写日志头
echo LINK_MIGRATE_LOG > "!LOG_FILE!"
echo DATE: %date% %time% >> "!LOG_FILE!"
echo SOURCE: !SOURCE_BASE! >> "!LOG_FILE!"
echo TARGET: !TARGET_BASE! >> "!LOG_FILE!"
echo EXCLUDE: !EXCLUDE_LIST! >> "!LOG_FILE!"
echo. >> "!LOG_FILE!"

:: 遍历迁移
for /d %%D in ("!SOURCE_BASE!\*") do (
    set "FOLDER=%%~nxD"
    set "SKIP="

    for /f "delims=|" %%e in ("!EXCLUDE_LIST!") do (
        if /i "%%e"=="!FOLDER!" set "SKIP=1"
    )

    if defined SKIP (
        echo [跳过] !FOLDER!
    ) else (
        echo [处理] !FOLDER!
        set "SRC_DIR=%%~fD"
        set "DST_DIR=!TARGET_BASE!\!FOLDER!"

        if exist "!DST_DIR!\" (
            echo   [警告] 目标已存在，跳过以避免覆盖：!DST_DIR!
        ) else (
            robocopy "!SRC_DIR!" "!DST_DIR!" /E /MOVE /NFL /NDL /NJH /NJS /NS /NC
            set "RC=!errorlevel!"

            if !RC! GEQ 8 (
                echo   [失败] 复制出错（robocopy 代码 !RC!），已保留原目录，未创建链接。
            ) else (
                mklink /d "!SRC_DIR!" "!DST_DIR!" >nul 2>&1
                if errorlevel 1 (
                    echo   [失败] 创建符号链接失败，正在回滚……
                    robocopy "!DST_DIR!" "!SRC_DIR!" /E /MOVE /NJH /NJS /NS /NC >nul 2>&1
                    rd "!DST_DIR!" /q /s >nul 2>&1
                    echo   [已回滚] 数据已退回原目录。
                ) else (
                    echo   [完成] 已迁移并创建软链接。
                    echo LINK^|!SRC_DIR!^|!DST_DIR! >> "!LOG_FILE!"
                )
            )
        )
    )
)

echo.
echo 迁移完成！日志：!LOG_FILE!
pause >nul
exit /b


::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:: 2. 恢复模块（从日志恢复）
::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
:RESTORE
cls
echo.
set /p "LOG_DIR=请输入日志所在目录（目标根目录）："
if "!LOG_DIR:~-1!"=="\" set "LOG_DIR=!LOG_DIR:~0,-1!"
set "LOG_FILE=!LOG_DIR!\link_restore_log.txt"

if not exist "!LOG_FILE!" (
    echo 未找到日志：!LOG_FILE!
    pause >nul
    exit /b
)

echo.
echo 找到日志，开始恢复……
echo.

for /f "usebackq delims=| tokens=1,2,3" %%a in (`findstr /b /c:"LINK|" "!LOG_FILE!"`) do (
    set "src=%%b"
    set "dst=%%c"

    echo 恢复：!src!

    if not exist "!dst!\" (
        echo   [跳过] 数据源不存在：!dst!
    ) else (
        :: 先移除符号链接（仅删链接本身，不删内容）
        if exist "!src!\" (
            rmdir "!src!" 2>nul
        )
        if exist "!src!\" (
            echo   [跳过] 原位置仍存在实体目录，未清理：!src!
        ) else (
            robocopy "!dst!" "!src!" /E /MOVE /NJH /NJS /NS /NC
            set "RC=!errorlevel!"
            if !RC! GEQ 8 (
                echo   [失败] 恢复出错（robocopy 代码 !RC!），数据仍保留在：!dst!
            ) else (
                rd "!dst!" /q /s >nul 2>&1
                echo   [完成] 已恢复回原位置。
            )
        )
    )
)

echo.
echo 全部恢复完成！
pause >nul
exit /b