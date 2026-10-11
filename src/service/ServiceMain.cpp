#include "service/ServiceMain.h"
#include "service/ServiceWorker.h"
#include "core/PathUtil.h"
#include "core/Config.h"
#include "core/Log.h"

#include <windows.h>
#include <string>

namespace dirmv
{

    static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
    static SERVICE_STATUS g_status{};
    static ServiceWorker *g_worker = nullptr;

    static void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0)
    {
        static DWORD checkpoint = 1;
        g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_status.dwCurrentState = state;
        g_status.dwWin32ExitCode = exitCode;
        g_status.dwWaitHint = waitHint;
        g_status.dwControlsAccepted = (state == SERVICE_START_PENDING) ? 0
                                                                       : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE);
        if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
            g_status.dwCheckPoint = checkpoint++;
        else
            g_status.dwCheckPoint = 0;
        if (g_statusHandle)
            SetServiceStatus(g_statusHandle, &g_status);
    }

    // 服务控制回调：响应停止/暂停/继续
    static DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID)
    {
        switch (ctrl)
        {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (g_worker)
                g_worker->RequestStop();
            return NO_ERROR;
        case SERVICE_CONTROL_PAUSE:
            if (g_worker)
                g_worker->RequestPause();
            ReportStatus(SERVICE_PAUSED);
            return NO_ERROR;
        case SERVICE_CONTROL_CONTINUE:
            if (g_worker)
                g_worker->RequestResume();
            ReportStatus(SERVICE_RUNNING);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    static void WINAPI ServiceMainImpl(DWORD, LPWSTR *)
    {
        std::wstring svcName = L"dirMoverSvc";
        Config cfg;
        ConfigStore::Load(ConfigStore::DefaultPath(), cfg);
        if (!cfg.serviceName.empty())
            svcName = cfg.serviceName;

        g_statusHandle = RegisterServiceCtrlHandlerExW(svcName.c_str(),
                                                       ServiceCtrlHandler, nullptr);
        if (!g_statusHandle)
            return;

        ReportStatus(SERVICE_START_PENDING, NO_ERROR, 8000);

        std::wstring logPath = Path::Combine(Path::ExeDir(), L"dirmv_service.log");
        Logger::Instance().SetFile(logPath);
        Logger::Instance().Info(L"服务启动");

        ServiceWorker worker;
        g_worker = &worker;
        ReportStatus(SERVICE_RUNNING);

        worker.Run(); // 阻塞直到停止

        g_worker = nullptr;
        ReportStatus(SERVICE_STOPPED);
        Logger::Instance().Info(L"服务已停止");
    }

    int RunServiceDispatcher()
    {
        SERVICE_TABLE_ENTRYW table[] = {
            {const_cast<LPWSTR>(L"dirMoverSvc"), ServiceMainImpl},
            {nullptr, nullptr}};
        if (!StartServiceCtrlDispatcherW(table))
        {
            DWORD e = GetLastError();
            if (e == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            {
                MessageBoxW(nullptr,
                            L"此模式需由 Windows 服务管理器启动。\n"
                            L"请使用 GUI 的『安装服务』按钮，或运行:\n"
                            L"  sc start dirMoverSvc",
                            L"dirMover", MB_OK | MB_ICONINFORMATION);
            }
            return 1;
        }
        return 0;
    }

} // namespace dirmv
