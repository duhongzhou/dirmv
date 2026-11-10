#include "service/ServiceMain.h"
#include "core/PathUtil.h"

#include <windows.h>
#include <string>

namespace dirmv
{

    bool ServiceInstaller::Install(const std::wstring &serviceName,
                                   const std::wstring &displayName, std::wstring *error)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm)
        {
            if (error)
                *error = L"无法打开服务管理器（需管理员权限）";
            return false;
        }
        // 服务宿主应为同目录下的 dirMover.exe：
        // 通过 dirMover_cli.exe 安装时也不能把服务注册成控制台程序。
        std::wstring hostExe = Path::ExePath();
        std::wstring guiExe = Path::Combine(Path::ExeDir(), L"dirMover.exe");
        if (Path::FileExists(guiExe))
            hostExe = guiExe;
        std::wstring binPath = L"\"" + hostExe + L"\" --service";

        SC_HANDLE svc = CreateServiceW(
            scm, serviceName.c_str(), displayName.c_str(),
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

        if (!svc)
        {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_EXISTS)
            {
                svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_ALL_ACCESS);
                if (svc)
                    ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                         SERVICE_NO_CHANGE, binPath.c_str(),
                                         nullptr, nullptr, nullptr, nullptr, nullptr,
                                         displayName.c_str());
            }
            else
            {
                wchar_t buf[128];
                swprintf_s(buf, L"创建服务失败(错误 %lu，需管理员权限)", e);
                if (error)
                    *error = buf;
                CloseServiceHandle(scm);
                return false;
            }
        }

        if (svc)
        {
            SERVICE_DESCRIPTIONW desc{};
            desc.lpDescription = const_cast<LPWSTR>(
                L"监视目录并在后台自动/定时把非白名单目录迁移到目标目录并创建链接");
            ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
            SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
            ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
            CloseServiceHandle(svc);
        }
        CloseServiceHandle(scm);
        return true;
    }

    bool ServiceInstaller::Uninstall(const std::wstring &serviceName, std::wstring *error)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
        {
            if (error)
                *error = L"无法打开服务管理器";
            return false;
        }
        SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_ALL_ACCESS);
        if (!svc)
        {
            CloseServiceHandle(scm);
            if (error)
                *error = L"服务不存在";
            return false;
        }
        SERVICE_STATUS st{};
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        bool ok = DeleteService(svc) != FALSE;
        if (!ok && error)
            *error = L"删除服务失败（需管理员权限）";
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok;
    }

    bool ServiceInstaller::Start(const std::wstring &serviceName, std::wstring *error)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
        {
            if (error)
                *error = L"无法打开服务管理器（需管理员权限）";
            return false;
        }
        SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_START);
        if (!svc)
        {
            CloseServiceHandle(scm);
            if (error)
                *error = L"服务不存在";
            return false;
        }
        BOOL ok = StartServiceW(svc, 0, nullptr);
        if (!ok && error)
        {
            wchar_t buf[128];
            swprintf_s(buf, L"启动服务失败(错误 %lu)", GetLastError());
            *error = buf;
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok != FALSE;
    }

    bool ServiceInstaller::Stop(const std::wstring &serviceName, std::wstring *error)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
        {
            if (error)
                *error = L"无法打开服务管理器（需管理员权限）";
            return false;
        }
        SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_STOP);
        if (!svc)
        {
            CloseServiceHandle(scm);
            if (error)
                *error = L"服务不存在";
            return false;
        }
        SERVICE_STATUS st{};
        BOOL ok = ControlService(svc, SERVICE_CONTROL_STOP, &st);
        if (!ok && error)
        {
            wchar_t buf[128];
            swprintf_s(buf, L"停止服务失败(错误 %lu)", GetLastError());
            *error = buf;
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok != FALSE;
    }

    bool ServiceInstaller::Control(const std::wstring &serviceName, DWORD code,
                                   std::wstring *error)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
        {
            if (error)
                *error = L"无法打开服务管理器";
            return false;
        }
        SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(),
                                     SERVICE_START | SERVICE_STOP);
        if (!svc)
        {
            CloseServiceHandle(scm);
            if (error)
                *error = L"服务不存在";
            return false;
        }
        SERVICE_STATUS st{};
        BOOL ok = StartServiceW(svc, 0, nullptr);
        if (!ok)
        {
            DWORD e = GetLastError();
            ok = ControlService(svc, code, &st);
            if (!ok && error)
            {
                wchar_t buf[128];
                swprintf_s(buf, L"操作服务失败(错误 %lu)", e);
                *error = buf;
            }
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok != FALSE;
    }

    bool ServiceInstaller::IsInstalled(const std::wstring &serviceName)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return false;
        SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS);
        bool ok = svc != nullptr;
        if (svc)
            CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return ok;
    }

    int ServiceInstaller::QueryState(const std::wstring &serviceName)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm)
            return -1;
        SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(), SERVICE_QUERY_STATUS);
        if (!svc)
        {
            CloseServiceHandle(scm);
            return -2; // 服务不存在（未安装）
        }
        SERVICE_STATUS st{};
        int result = -1;
        if (QueryServiceStatus(svc, &st))
        {
            switch (st.dwCurrentState)
            {
            case SERVICE_RUNNING:
                result = 1;
                break;
            case SERVICE_PAUSED:
                result = 3;
                break;
            case SERVICE_STOPPED:
                result = 0;
                break;
            case SERVICE_START_PENDING:
            case SERVICE_STOP_PENDING:
            case SERVICE_CONTINUE_PENDING:
            case SERVICE_PAUSE_PENDING:
                result = 2;
                break;
            default:
                result = 2;
                break;
            }
        }
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return result;
    }

} // namespace dirmv
