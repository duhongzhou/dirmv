#include "core/PipeServer.h"
#include "core/PathUtil.h"
#include "core/Log.h"

#include <windows.h>
#include <sddl.h>
#include <vector>

namespace dirmv
{

    const wchar_t *PipeCommandToString(PipeCommand c)
    {
        switch (c)
        {
        case PipeCommand::Status:
            return L"status";
        case PipeCommand::Migrate:
            return L"migrate";
        case PipeCommand::Restore:
            return L"restore";
        case PipeCommand::Stop:
            return L"stop";
        case PipeCommand::Reload:
            return L"reload";
        default:
            return L"unknown";
        }
    }

    PipeCommand PipeCommandFromString(const std::wstring &s)
    {
        if (s == L"status")
            return PipeCommand::Status;
        if (s == L"migrate")
            return PipeCommand::Migrate;
        if (s == L"restore")
            return PipeCommand::Restore;
        if (s == L"stop")
            return PipeCommand::Stop;
        if (s == L"reload")
            return PipeCommand::Reload;
        return PipeCommand::Unknown;
    }

    std::wstring PipeServer::DefaultPipeName()
    {
        return L"\\\\.\\pipe\\dirMoverControl";
    }

    void PipeServer::Start(const std::wstring &pipeName,
                           std::function<std::wstring(PipeCommand, const std::wstring &)> handler)
    {
        if (running_.load())
            return;
        pipeName_ = pipeName;
        handler_ = std::move(handler);
        running_ = true;
        thread_ = std::thread(&PipeServer::Run, this);
    }

    void PipeServer::Stop()
    {
        if (!running_.exchange(false))
        {
            if (thread_.joinable())
                thread_.join();
            return;
        }
        if (stopEvent_)
            SetEvent((HANDLE)stopEvent_);
        // 唤醒阻塞中的 ConnectNamedPipe
        HANDLE h = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);

        if (thread_.joinable())
            thread_.join();
        if (stopEvent_)
        {
            CloseHandle((HANDLE)stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    void PipeServer::Run()
    {
        HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stopEvent_ = hStop;

        while (running_.load())
        {
            // 允许 Everyone 读写（服务与普通权限 GUI 通信）
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = FALSE;
            PSECURITY_DESCRIPTOR sd = nullptr;
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr);
            sa.lpSecurityDescriptor = sd;

            HANDLE hPipe = CreateNamedPipeW(
                pipeName_.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES, 8192, 8192, 0, &sa);
            if (sd)
                LocalFree(sd);

            if (hPipe == INVALID_HANDLE_VALUE)
            {
                LOGW(L"创建命名管道失败，稍后重试");
                WaitForSingleObject(hStop, 1000);
                continue;
            }

            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            BOOL connected = ConnectNamedPipe(hPipe, &ov);
            DWORD err = GetLastError();
            if (!connected && err == ERROR_PIPE_CONNECTED)
            {
                connected = TRUE;
            }
            else if (!connected && err == ERROR_IO_PENDING)
            {
                HANDLE waits[2] = {ov.hEvent, hStop};
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0)
                {
                    DWORD dummy = 0;
                    GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
                    connected = TRUE;
                }
            }
            CloseHandle(ov.hEvent);

            if (!running_.load() || !connected)
            {
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
                continue;
            }

            // 读取一行请求
            char buf[1024]{};
            DWORD read = 0;
            if (ReadFile(hPipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0)
            {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)read, nullptr, 0);
                std::wstring req(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, buf, (int)read, &req[0], wlen);
                req = Path::Trim(req);

                size_t sp = req.find(L' ');
                std::wstring cmd = (sp == std::wstring::npos) ? req : req.substr(0, sp);
                std::wstring arg = (sp == std::wstring::npos) ? L"" : req.substr(sp + 1);

                PipeCommand pc = PipeCommandFromString(cmd);
                std::wstring resp = handler_ ? handler_(pc, arg) : L"{\"ok\":false}";

                DWORD written = 0;
                int utf8len = WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), (int)resp.size(),
                                                  nullptr, 0, nullptr, nullptr);
                std::string out((size_t)utf8len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, resp.c_str(), (int)resp.size(),
                                    &out[0], utf8len, nullptr, nullptr);
                WriteFile(hPipe, out.data(), (DWORD)out.size(), &written, nullptr);
                FlushFileBuffers(hPipe);
            }

            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }
        running_ = false;
    }

    // ---------------------------------------------------------------------------
    std::wstring PipeClient::Send(const std::wstring &pipeName, PipeCommand cmd,
                                  const std::wstring &arg)
    {
        if (!WaitNamedPipeW(pipeName.c_str(), 2000))
        {
            return L"";
        }
        HANDLE h = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return L"";

        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);

        std::wstring req = PipeCommandToString(cmd);
        if (!arg.empty())
        {
            req += L" ";
            req += arg;
        }

        int utf8len = WideCharToMultiByte(CP_UTF8, 0, req.c_str(), (int)req.size(),
                                          nullptr, 0, nullptr, nullptr);
        std::string out((size_t)utf8len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, req.c_str(), (int)req.size(),
                            &out[0], utf8len, nullptr, nullptr);

        DWORD written = 0;
        if (!WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr))
        {
            CloseHandle(h);
            return L"";
        }
        FlushFileBuffers(h);

        char buf[8192]{};
        DWORD read = 0;
        std::wstring resp;
        if (ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && read > 0)
        {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, (int)read, nullptr, 0);
            resp.resize(wlen);
            MultiByteToWideChar(CP_UTF8, 0, buf, (int)read, &resp[0], wlen);
        }
        CloseHandle(h);
        return resp;
    }

} // namespace dirmv
