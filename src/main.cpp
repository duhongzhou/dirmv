#include "gui/App.h"
#include "service/ServiceMain.h"
#include "core/CliRunner.h"
#include "core/Config.h"

#include <windows.h>
#include <string>
#include <cwchar>

// 判断命令行是否包含指定开关（不区分大小写）
static bool HasSwitch(const std::wstring &cmdLine, const wchar_t *sw)
{
    std::wstring lower = cmdLine;
    for (auto &c : lower)
        c = (wchar_t)towlower(c);
    std::wstring s = sw;
    for (auto &c : s)
        c = (wchar_t)towlower(c);
    return lower.find(s) != std::wstring::npos;
}

// 从命令行解析 --config <path>（支持引号）
static std::wstring ParseConfigArg(const std::wstring &cmd)
{
    size_t pos = 0;
    while (pos < cmd.size())
    {
        size_t f = cmd.find(L"--config", pos);
        if (f == std::wstring::npos)
            break;
        // 要求后面是空白或结束
        size_t after = f + 8;
        if (after < cmd.size() && cmd[after] != L' ' && cmd[after] != L'\t')
        {
            pos = after;
            continue;
        }
        size_t i = after;
        while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t'))
            ++i;
        if (i >= cmd.size())
            break;
        std::wstring val;
        if (cmd[i] == L'"')
        {
            size_t j = cmd.find(L'"', i + 1);
            if (j == std::wstring::npos)
                val = cmd.substr(i + 1);
            else
                val = cmd.substr(i + 1, j - i - 1);
        }
        else
        {
            size_t j = i;
            while (j < cmd.size() && cmd[j] != L' ' && cmd[j] != L'\t')
                ++j;
            val = cmd.substr(i, j - i);
        }
        return val;
    }
    return L"";
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
    std::wstring cmd = lpCmdLine ? lpCmdLine : L"";

    if (HasSwitch(cmd, L"--service"))
    {
        // 由 SCM 启动的服务模式
        return dirmv::RunServiceDispatcher();
    }
    if (HasSwitch(cmd, L"--cli") || HasSwitch(cmd, L"--console"))
    {
        // 控制台模式（迁移/恢复/服务管理）。
        // lpCmdLine 含开关本身（如 "--cli migrate ..."），RunCli 期望首 token 是命令，
        // 因此先去掉 --cli / --console 开关再转发。
        std::wstring cli = cmd;
        for (const wchar_t *sw : {L"--cli", L"--console"})
        {
            size_t pos = cli.find(sw);
            if (pos != std::wstring::npos)
            {
                cli.erase(pos, wcslen(sw));
                break;
            }
        }
        return dirmv::RunCli(cli);
    }

    // GUI：支持 --config <path> 指定配置文件（测试时指向沙箱）
    std::wstring cfgPath = ParseConfigArg(cmd);
    if (!cfgPath.empty())
        dirmv::ConfigStore::SetOverridePath(cfgPath);

    return dirmv::gui::RunApp(hInstance, nCmdShow);
}
