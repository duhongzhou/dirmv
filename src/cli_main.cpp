// ---------------------------------------------------------------------------
// cli_main.cpp - 独立控制台可执行入口（逻辑在 core/CliRunner.cpp）
// ---------------------------------------------------------------------------
#include "core/CliRunner.h"

#include <windows.h>
#include <string>

int wmain(int argc, wchar_t **argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::wstring cmd;
    for (int i = 1; i < argc; ++i)
    {
        if (i > 1)
            cmd += L" ";
        cmd += argv[i];
    }
    return dirmv::RunCli(cmd);
}
