#pragma once
// ---------------------------------------------------------------------------
// ServiceWorker.h - 服务后台工作体
//   常驻循环：定时调度(周期+时刻) + 文件系统监视 + 命名管道控制
//   占用重试：检测到占用 → 通知 + 创建一次性重试计划任务(不重复创建)
// ---------------------------------------------------------------------------
#include "core/Config.h"
#include "core/MigrateEngine.h"
#include "core/DirectoryWatcher.h"
#include "core/PipeServer.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>

namespace dirmv
{

    class ServiceWorker
    {
    public:
        ServiceWorker();
        ~ServiceWorker();

        // 阻塞运行直到 RequestStop
        void Run();

        void RequestStop();
        void RequestPause();
        void RequestResume();

    private:
        void ReloadConfig();
        void TriggerMigrate(bool async, const std::wstring &onlyWatchDir = L"");
        std::wstring HandlePipe(PipeCommand cmd, const std::wstring &arg);
        void OnLockNotice(const LockNotice &notice);
        // 依据当前配置（仅正确可用的规则）重建文件系统监视器
        void SetupWatchers();

        Config cfg_;
        std::mutex cfgMtx_;

        std::atomic<bool> stopped_{false};
        std::atomic<bool> paused_{false};
        std::atomic<bool> busy_{false};
        std::atomic<bool> cancel_{false};

        std::vector<std::unique_ptr<DirectoryWatcher>> watchers_;
        std::unique_ptr<PipeServer> pipe_;

        std::mutex lastRunMtx_;
        std::wstring lastRunInfo_ = L"未运行";

        std::thread migrateThread_;
        std::mutex migrateThreadMtx_;

        std::time_t nextRun_ = 0; // 下一次定时运行时刻
    };

} // namespace dirmv
