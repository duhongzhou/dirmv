#include "core/FileEngine.h"
#include "core/PathUtil.h"
#include "core/Log.h"

#include <windows.h>
#include <restartmanager.h>
#include <vector>
#include <stack>

namespace dirmv
{

    // -------------------- 辅助：遍历目录树 --------------------
    namespace
    {

        struct TreeEntry
        {
            std::wstring srcPath;
            std::wstring relPath; // 相对 src 根的路径
            bool isDir;
            uint64_t size;
            FILETIME writeTime;
            DWORD attributes;
        };

        // 收集整棵树的条目（不含根），使用迭代避免深递归栈溢出
        std::vector<TreeEntry> CollectTree(const std::wstring &srcRoot,
                                           std::atomic<bool> *cancel)
        {
            std::vector<TreeEntry> entries;
            std::stack<std::pair<std::wstring, std::wstring>> stk; // (磁盘路径, 相对路径)
            stk.push({srcRoot, L""});

            while (!stk.empty())
            {
                if (cancel && cancel->load())
                    break;
                auto [dir, rel] = stk.top();
                stk.pop();

                std::wstring pattern = Path::Combine(dir, L"*");
                WIN32_FIND_DATAW fd{};
                HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                            FindExSearchNameMatch, nullptr,
                                            FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                do
                {
                    const wchar_t *name = fd.cFileName;
                    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
                        continue;

                    TreeEntry e{};
                    e.srcPath = Path::Combine(dir, name);
                    e.relPath = rel.empty() ? name : Path::Combine(rel, name);
                    e.attributes = fd.dwFileAttributes;
                    e.writeTime = fd.ftLastWriteTime;

                    // 跳过重解析点（软链接/联接），避免复制链接本身或递归
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                        continue;

                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        e.isDir = true;
                        e.size = 0;
                        entries.push_back(e);
                        stk.push({e.srcPath, e.relPath});
                    }
                    else
                    {
                        e.isDir = false;
                        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                        entries.push_back(e);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            return entries;
        }

        // CopyFileEx 进度回调
        struct CopyCtx
        {
            ProgressFn *onProgress;
            std::atomic<bool> *cancel;
            uint64_t baseDone;
            uint64_t totalBytes;
            const std::wstring *currentName;
            bool canceled;
        };

        DWORD CALLBACK CopyProgressRoutine(LARGE_INTEGER totalSize, LARGE_INTEGER totalTransferred,
                                           LARGE_INTEGER, LARGE_INTEGER,
                                           DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
        {
            (void)totalSize;
            auto *ctx = static_cast<CopyCtx *>(data);
            if (ctx->cancel && ctx->cancel->load())
            {
                ctx->canceled = true;
                return PROGRESS_CANCEL;
            }
            if (*ctx->onProgress)
            {
                (*ctx->onProgress)(ctx->baseDone + (uint64_t)totalTransferred.QuadPart,
                                   ctx->totalBytes,
                                   ctx->currentName ? *ctx->currentName : L"");
            }
            return PROGRESS_CONTINUE;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    bool FileEngine::Scan(const std::wstring &dir, DirStat &out, std::atomic<bool> *cancel)
    {
        out = DirStat{};
        auto entries = CollectTree(dir, cancel);
        for (auto &e : entries)
        {
            if (e.isDir)
                out.dirCount++;
            else
            {
                out.fileCount++;
                out.totalBytes += e.size;
            }
        }
        return true;
    }

    bool FileEngine::EnsureDirectory(const std::wstring &dir)
    {
        if (Path::DirectoryExists(dir))
            return true;
        // 递归创建
        std::wstring parent = Path::ParentDir(dir);
        if (!parent.empty() && !Path::DirectoryExists(parent))
            EnsureDirectory(parent);
        if (CreateDirectoryW(dir.c_str(), nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    // ---------------------------------------------------------------------------
    // 复制并（可选）删除源。deleteSrc 决定 MoveTree / CopyTree 语义。
    static int CopyTreeImpl(const std::wstring &src, const std::wstring &dst,
                            bool deleteSrc, ProgressFn onProgress,
                            std::atomic<bool> *cancel, std::wstring *firstError)
    {
        int failures = 0;
        auto entries = CollectTree(src, cancel);
        if (cancel && cancel->load())
            return 1;

        uint64_t total = 0;
        for (auto &e : entries)
            if (!e.isDir)
                total += e.size;

        if (!FileEngine::EnsureDirectory(dst))
        {
            if (firstError)
                *firstError = L"无法创建目标目录: " + dst;
            return 1;
        }

        // 先建目录，再复制文件
        for (auto &e : entries)
        {
            if (cancel && cancel->load())
                break;
            if (!e.isDir)
                continue;
            std::wstring targetDir = dirmv::Path::Combine(dst, e.relPath);
            FileEngine::EnsureDirectory(targetDir);
        }

        uint64_t done = 0;
        for (auto &e : entries)
        {
            if (cancel && cancel->load())
                break;
            if (e.isDir)
                continue;

            std::wstring targetFile = dirmv::Path::Combine(dst, e.relPath);

            CopyCtx ctx{&onProgress, cancel, done, total, &e.relPath, false};
            BOOL ok = CopyFileExW(e.srcPath.c_str(), targetFile.c_str(),
                                  onProgress ? CopyProgressRoutine : nullptr,
                                  onProgress ? &ctx : nullptr,
                                  nullptr, 0);
            if (ctx.canceled)
                break;

            if (!ok)
            {
                ++failures;
                if (firstError && firstError->empty())
                {
                    wchar_t buf[64];
                    swprintf_s(buf, L"CopyFileEx 失败(错误 %lu): %s",
                               GetLastError(), e.srcPath.c_str());
                    *firstError = buf;
                }
                // 保留时间戳
            }
            else
            {
                // 复制后同步时间戳
                HANDLE h = CreateFileW(targetFile.c_str(), FILE_WRITE_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    SetFileTime(h, nullptr, nullptr, &e.writeTime);
                    CloseHandle(h);
                }
            }
            done += e.size;
            if (onProgress)
                onProgress(done, total, e.relPath);
        }

        // 删除源（移动语义）：先删文件，后删目录（自底向上）
        if (deleteSrc && failures == 0 && !(cancel && cancel->load()))
        {
            for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            {
                if (!it->isDir)
                    DeleteFileW(it->srcPath.c_str());
            }
            for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            {
                if (it->isDir)
                    RemoveDirectoryW(it->srcPath.c_str());
            }
            RemoveDirectoryW(src.c_str());
        }
        return failures;
    }

    int FileEngine::CopyTree(const std::wstring &src, const std::wstring &dst,
                             ProgressFn onProgress, std::atomic<bool> *cancel,
                             std::wstring *firstError)
    {
        return CopyTreeImpl(src, dst, false, onProgress, cancel, firstError);
    }

    int FileEngine::MoveTree(const std::wstring &src, const std::wstring &dst,
                             ProgressFn onProgress, std::atomic<bool> *cancel,
                             std::wstring *firstError)
    {
        return CopyTreeImpl(src, dst, true, onProgress, cancel, firstError);
    }

    // ---------------------------------------------------------------------------
    bool FileEngine::RemoveTree(const std::wstring &dir, std::wstring *firstError)
    {
        auto entries = CollectTree(dir, nullptr);
        bool ok = true;
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            if (!it->isDir)
            {
                if (!DeleteFileW(it->srcPath.c_str()))
                {
                    // 尝试清除只读属性后重试
                    SetFileAttributesW(it->srcPath.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!DeleteFileW(it->srcPath.c_str()))
                    {
                        ok = false;
                        if (firstError && firstError->empty())
                            *firstError = L"删除文件失败: " + it->srcPath;
                    }
                }
            }
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        {
            if (it->isDir)
                RemoveDirectoryW(it->srcPath.c_str());
        }
        if (!RemoveDirectoryW(dir.c_str()))
        {
            DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND)
            {
                ok = false;
                if (firstError && firstError->empty())
                    *firstError = L"删除目录失败: " + dir;
            }
        }
        return ok;
    }

    // ---------------------------------------------------------------------------
    bool FileEngine::CreateLink(const std::wstring &linkPath, const std::wstring &targetPath,
                                bool useJunction, std::wstring *error)
    {
        if (useJunction)
        {
            // 目录联接：目标必须是绝对路径
            return CreateSymbolicLinkW(linkPath.c_str(), targetPath.c_str(),
                                       SYMBOLIC_LINK_FLAG_DIRECTORY) != FALSE;
        }
        // 目录符号链接（等价 mklink /d），需要管理员或开发者模式
        DWORD flags = SYMBOLIC_LINK_FLAG_DIRECTORY;
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
        flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
        if (CreateSymbolicLinkW(linkPath.c_str(), targetPath.c_str(), flags))
            return true;

        DWORD e = GetLastError();
        // 回退：不带 UNPRIVILEGED 标志（部分旧系统不识别该标志）
        if (CreateSymbolicLinkW(linkPath.c_str(), targetPath.c_str(),
                                SYMBOLIC_LINK_FLAG_DIRECTORY))
            return true;

        if (error)
        {
            wchar_t buf[128];
            swprintf_s(buf, L"CreateSymbolicLink 失败(错误 %lu)，需管理员权限", e);
            *error = buf;
        }
        return false;
    }

    bool FileEngine::RemoveLink(const std::wstring &linkPath, std::wstring *error)
    {
        DWORD attr = GetFileAttributesW(linkPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES)
            return true;
        // 删除重解析点本身
        if (RemoveDirectoryW(linkPath.c_str()))
            return true;
        if (DeleteFileW(linkPath.c_str()))
            return true;
        if (error)
        {
            wchar_t buf[128];
            swprintf_s(buf, L"删除链接失败(错误 %lu): %s", GetLastError(), linkPath.c_str());
            *error = buf;
        }
        return false;
    }

    bool FileEngine::IsEmptyDir(const std::wstring &dir)
    {
        std::wstring pattern = dirmv::Path::Combine(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return true;
        bool empty = true;
        do
        {
            if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
            {
                empty = false;
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return empty;
    }

    bool FileEngine::IsElevated()
    {
        BOOL elevated = FALSE;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            TOKEN_ELEVATION elev{};
            DWORD sz = sizeof(elev);
            if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
                elevated = elev.TokenIsElevated;
            CloseHandle(token);
        }
        return elevated != FALSE;
    }

    // ---------------------------------------------------------------------------
    // 占用检测
    // ---------------------------------------------------------------------------
    bool FileEngine::IsFileLocked(const std::wstring &file)
    {
        // 以「独占、不共享」方式尝试打开；失败且为共享冲突 => 被占用
        HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, // 不共享
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            return false;
        }
        DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION)
            return true;
        // 其他错误（权限等）不视为锁定，避免误报
        return false;
    }

    std::vector<std::wstring> FileEngine::QueryLockingProcesses(const std::wstring &file)
    {
        std::vector<std::wstring> result;
        // 使用 Restart Manager 查询占用该文件的进程。
        // 注意：必须静态链接 Rstrtmgr.lib 并直接调用 API；
        // 通过 LoadLibrary+GetProcAddress 调用会崩溃。
        DWORD session = 0;
        WCHAR sessionKey[CCH_RM_SESSION_KEY + 1] = {0};
        if (RmStartSession(&session, 0, sessionKey) != ERROR_SUCCESS)
            return result;

        const WCHAR *files[1] = {file.c_str()};
        if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS)
        {
            UINT needed = 0;
            UINT have = 0;
            DWORD reason = 0;
            DWORD rc = RmGetList(session, &needed, &have, nullptr, &reason);
            if (rc == ERROR_MORE_DATA && needed > 0)
            {
                std::vector<RM_PROCESS_INFO> info(needed);
                have = needed;
                if (RmGetList(session, &needed, &have, info.data(), &reason) == ERROR_SUCCESS)
                {
                    for (UINT i = 0; i < have; ++i)
                    {
                        std::wstring name = info[i].strAppName;
                        wchar_t buf[64];
                        swprintf_s(buf, L" (PID %lu)", info[i].Process.dwProcessId);
                        result.push_back(name + buf);
                    }
                }
            }
        }
        RmEndSession(session);
        return result;
    }

    std::vector<LockInfo> FileEngine::DetectLocks(const std::wstring &dir)
    {
        std::vector<LockInfo> locks;
        auto entries = CollectTree(dir, nullptr);
        for (auto &e : entries)
        {
            if (e.isDir)
                continue;
            if (IsFileLocked(e.srcPath))
            {
                LockInfo li;
                li.filePath = e.srcPath;
                li.processes = QueryLockingProcesses(e.srcPath);
                locks.push_back(std::move(li));
            }
        }
        return locks;
    }

} // namespace dirmv
