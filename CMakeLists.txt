cmake_minimum_required(VERSION 3.20)

project(dirMover
    VERSION 1.1.3
    DESCRIPTION "目录软链接迁移/恢复工具 - 将非白名单目录迁移到目标目录并创建链接"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # 使用 UTF-8 源码与执行字符集，避免中文乱码
    add_compile_options(/utf-8 /W4 /MP /permissive-)
    add_compile_definitions(
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )

    # 静态链接 CRT，生成免依赖的单文件 exe
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# 通用库：核心逻辑（迁移引擎、配置、日志、服务、管道）
# ---------------------------------------------------------------------------
add_library(dirmv_core STATIC
    src/core/Log.cpp
    src/core/Config.cpp
    src/core/PathUtil.cpp
    src/core/FileEngine.cpp
    src/core/MigrateEngine.cpp
    src/core/DirectoryWatcher.cpp
    src/core/PipeServer.cpp
    src/core/ServiceInstaller.cpp
    src/core/Scheduler.cpp
    src/core/Integrity.cpp
    src/core/CliRunner.cpp
)

target_include_directories(dirmv_core PUBLIC src)

target_link_libraries(dirmv_core PUBLIC
    shlwapi
    shell32
    ole32
    advapi32
    user32
    uuid
    Rstrtmgr
)

# ---------------------------------------------------------------------------
# GUI / 服务主程序
# ---------------------------------------------------------------------------
add_executable(dirMover WIN32
    src/main.cpp
    src/gui/App.cpp
    src/gui/ProgressDialog.cpp
    src/gui/MainWindow.cpp
    src/gui/TrayIcon.cpp
    src/service/ServiceMain.cpp
    src/service/ServiceWorker.cpp
    src/resource.rc
)

target_link_libraries(dirMover PRIVATE
    dirmv_core
    comctl32
    comdlg32
    gdi32
    shell32
    shlwapi
    ole32
    advapi32
    user32
    uuid
    Wtsapi32
)

# 控制台版本，便于调试与脚本调用
add_executable(dirMover_cli
    src/cli_main.cpp
)
target_link_libraries(dirMover_cli PRIVATE dirmv_core)

# 控制台子系统（CLI 需要）
if(MSVC)
    target_link_options(dirMover_cli PRIVATE /SUBSYSTEM:CONSOLE)
endif()

# 应用图标：使用 assets/dirmv.ico（通过 DIRMV_ICON 宏传给 resource.rc）
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/dirmv.ico" DIRMV_ICON_PATH)

if(EXISTS "${DIRMV_ICON_PATH}")
    target_compile_definitions(dirMover PRIVATE
        DIRMV_ICON="${DIRMV_ICON_PATH}")
    set_source_files_properties(src/resource.rc PROPERTIES
        OBJECT_DEPENDS "${DIRMV_ICON_PATH}")
else()
    message(WARNING "未找到 assets/dirmv.ico，将使用默认图标名 dirmv.ico")
endif()

# ---------------------------------------------------------------------------
# 安装
# ---------------------------------------------------------------------------
install(TARGETS dirMover dirMover_cli RUNTIME DESTINATION .)
