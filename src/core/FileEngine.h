#pragma once
// ---------------------------------------------------------------------------
// FileEngine.h - 目录复制 / 移动 / 链接 / 删除 底层 API 封装
//   全部基于 Win32 API：FindFirstFileEx / CreateDirectory / CopyFileEx /
//   MoveFileEx / DeleteFile / RemoveDirectory / CreateSymbolicLink
// ---------------------------------------------------------------------------
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <atomic>

namespace dirmv
{

    struct DirStat
    {
        uint64_t totalBytes = 0;
        uint64_t fileCount = 0;
        uint64_t dirCount = 0;
    };

    // 描述一个被占用的文件及其占用者
    struct LockInfo
    {
        std::wstring filePath;               // 被占用的文件
        std::vector<std::wstring> processes; // 占用进程名(含PID)
    };

    // 进度回调：copiedBytes 已处理字节，(totalBytes>0 时可算百分比)
    using ProgressFn = std::function<void(uint64_t doneBytes, uint64_t totalBytes,
                                          const std::wstring &currentItem)>;

    class FileEngine
    {
    public:
        // 递归统计目录大小（不跟随重解析点）
        static bool Scan(const std::wstring &dir, DirStat &out,
                         std::atomic<bool> *cancel = nullptr);

        // 递归复制目录内容到目标（目标目录会被创建）。
        // 使用 CopyFileEx 获得进度；保留时间戳与属性。
        // 返回失败的文件数。
        static int CopyTree(const std::wstring &src, const std::wstring &dst,
                            ProgressFn onProgress,
                            std::atomic<bool> *cancel,
                            std::wstring *firstError);

        // 递归复制并删除源（= 移动语义，跨卷安全）
        static int MoveTree(const std::wstring &src, const std::wstring &dst,
                            ProgressFn onProgress,
                            std::atomic<bool> *cancel,
                            std::wstring *firstError);

        // 递归删除目录
        static bool RemoveTree(const std::wstring &dir, std::wstring *firstError);

        // 创建目录（含中间层）
        static bool EnsureDirectory(const std::wstring &dir);

        // 创建目录链接：isDir=true 时创建目录符号链接；useJunction=true 创建联接
        static bool CreateLink(const std::wstring &linkPath,
                               const std::wstring &targetPath,
                               bool useJunction,
                               std::wstring *error);

        // 删除符号链接/联接本身（不删除目标内容）
        static bool RemoveLink(const std::wstring &linkPath, std::wstring *error);

        // 目标目录是否为空
        static bool IsEmptyDir(const std::wstring &dir);

        // 判断当前进程是否具有管理员权限（创建符号链接所需）
        static bool IsElevated();

        // --- 占用检测 ---
        // 递归检查目录中是否有文件被其他进程占用（独占打开探测）。
        // 返回被占用文件列表；可用时用 RestartManager 填充占用进程名。
        static std::vector<LockInfo> DetectLocks(const std::wstring &dir);

        // 单个文件是否被占用（尝试以独占方式打开）
        static bool IsFileLocked(const std::wstring &file);

        // 用 RestartManager 查询占用某文件的进程名
        static std::vector<std::wstring> QueryLockingProcesses(const std::wstring &file);
    };

} // namespace dirmv
