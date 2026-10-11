#pragma once
// ---------------------------------------------------------------------------
// DirectoryWatcher.h - 基于 ReadDirectoryChangesW 的目录实时监视
//   当监视根目录下新增子目录时触发回调（例如软件自动创建的新数据目录）
// ---------------------------------------------------------------------------
#include <string>
#include <thread>
#include <atomic>
#include <functional>

namespace dirmv
{

    class DirectoryWatcher
    {
    public:
        DirectoryWatcher() = default;
        ~DirectoryWatcher() { Stop(); }

        // onCreate: 传入新增子目录的完整路径
        void Start(const std::wstring &root, std::function<void(const std::wstring &)> onCreate);
        void Stop();

        bool IsRunning() const { return running_.load(); }

    private:
        void Run();

        std::wstring root_;
        std::function<void(const std::wstring &)> onCreate_;
        std::thread thread_;
        std::atomic<bool> running_{false};
        void *stopEvent_ = nullptr; // HANDLE
    };

} // namespace dirmv
