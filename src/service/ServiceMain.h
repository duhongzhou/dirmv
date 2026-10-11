#pragma once
// ---------------------------------------------------------------------------
// ServiceMain.h - Windows 服务入口
//   同一 exe 既可为 GUI（无参数）也可作为服务运行（--service）
// ---------------------------------------------------------------------------
#include <windows.h>
#include <string>

namespace dirmv
{

    // 服务入口，由 wmain 在检测到 --service 时调用
    int RunServiceDispatcher();

    // 安装/卸载/启动/停止服务（GUI 与 CLI 使用）
    class ServiceInstaller
    {
    public:
        // 以当前 exe 路径与服务名安装（自动启动 = 开机启动）
        static bool Install(const std::wstring &serviceName, const std::wstring &displayName,
                            std::wstring *error);
        static bool Uninstall(const std::wstring &serviceName, std::wstring *error);
        static bool Start(const std::wstring &serviceName, std::wstring *error);
        static bool Stop(const std::wstring &serviceName, std::wstring *error);
        static bool IsInstalled(const std::wstring &serviceName);
        // 0=未安装/停止, 1=运行中, 2=启动中/停止中, -1=未知
        static int QueryState(const std::wstring &serviceName);

    private:
        static bool Control(const std::wstring &serviceName, DWORD code, std::wstring *error);
    };

} // namespace dirmv
