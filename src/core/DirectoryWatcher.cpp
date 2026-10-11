#include "core/DirectoryWatcher.h"
#include "core/PathUtil.h"
#include "core/Log.h"

#include <windows.h>
#include <vector>

namespace dirmv
{

    void DirectoryWatcher::Start(const std::wstring &root,
                                 std::function<void(const std::wstring &)> onCreate)
    {
        if (running_.load())
            return;
        root_ = root;
        onCreate_ = std::move(onCreate);
        running_ = true;
        thread_ = std::thread(&DirectoryWatcher::Run, this);
    }

    void DirectoryWatcher::Stop()
    {
        if (!running_.exchange(false))
        {
            if (thread_.joinable())
                thread_.join();
            return;
        }
        if (stopEvent_)
            SetEvent((HANDLE)stopEvent_);
        if (thread_.joinable())
            thread_.join();
        if (stopEvent_)
        {
            CloseHandle((HANDLE)stopEvent_);
            stopEvent_ = nullptr;
        }
    }

    void DirectoryWatcher::Run()
    {
        HANDLE hDir = CreateFileW(root_.c_str(), FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (hDir == INVALID_HANDLE_VALUE)
        {
            LOGE(L"无法监视目录(打开失败): " + root_);
            running_ = false;
            return;
        }

        HANDLE hStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        stopEvent_ = hStop;

        const DWORD BUF = 64 * 1024;
        std::vector<BYTE> buffer(BUF);

        while (running_.load())
        {
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

            BOOL ok = ReadDirectoryChangesW(
                hDir, buffer.data(), BUF, FALSE,
                FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME,
                nullptr, &ov, nullptr);

            if (!ok)
            {
                CloseHandle(ov.hEvent);
                break;
            }

            HANDLE waits[2] = {ov.hEvent, hStop};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

            if (w == WAIT_OBJECT_0)
            {
                DWORD bytes = 0;
                if (GetOverlappedResult(hDir, &ov, &bytes, FALSE) && bytes > 0)
                {
                    BYTE *p = buffer.data();
                    for (;;)
                    {
                        auto *info = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(p);
                        std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
                        std::wstring full = Path::Combine(root_, name);

                        if (info->Action == FILE_ACTION_ADDED ||
                            info->Action == FILE_ACTION_RENAMED_NEW_NAME)
                        {
                            if (Path::DirectoryExists(full) && onCreate_)
                            {
                                LOGI(L"[监视] 发现新目录: " + name);
                                onCreate_(full);
                            }
                        }
                        if (info->NextEntryOffset == 0)
                            break;
                        p += info->NextEntryOffset;
                    }
                }
            }
            else
            {
                CancelIo(hDir);
            }
            CloseHandle(ov.hEvent);
        }

        CloseHandle(hDir);
        running_ = false;
    }

} // namespace dirmv
