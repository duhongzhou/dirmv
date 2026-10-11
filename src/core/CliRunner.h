#pragma once
// ---------------------------------------------------------------------------
// CliRunner.h - 控制台命令实现（核心库）
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

namespace dirmv
{

    // 解析命令行并执行对应命令，返回退出码
    int RunCli(const std::wstring &cmdLine);
    int RunCliCommand(const std::vector<std::wstring> &args);

} // namespace dirmv
