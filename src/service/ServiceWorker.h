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
#include <functional>
#include <memory>
#include <vector>
#include <map>

namespace dirmv
{

    class ServiceWorker
    {
    public:
        ServiceWorker();
        ~ServiceWorker();

        // 阻塞运行直到 RequestStop
        // onStopWait：停止过程中仍有迁移任务在收尾时，每 200ms 回调一次，
        //             用于向 SCM 上报 STOP_PENDING 检查点，避免超时被强杀。
        void Run(std::function<void()> onStopWait = nullptr);

        void RequestStop();
        void RequestPause();
        void RequestResume();

    private:
        void ReloadConfig();
        void TriggerMigrate(bool async, const std::wstring &onlyWatchDir = L"",
                            bool manual = false);
        std::wstring HandlePipe(PipeCommand cmd, const std::wstring &arg);
        void OnLockNotice(const LockNotice &notice);
        // 依据当前配置（仅正确可用的规则）重建文件系统监视器
        void SetupWatchers();

        // 失败冷却：近期失败/被占用的子任务在同一源路径上暂时跳过，
        // 避免「监视触发 → 失败 → 再触发」的风暴式重试。
        bool CoolingDown(const std::wstring &srcPath);
        void CoolDown(const std::wstring &srcPath, int seconds);
        void ClearCooldowns();

        Config cfg_;
        std::mutex cfgMtx_;

        std::mutex cooldownMtx_;
        std::map<std::wstring, int64_t> cooldownUntil_; // 源路径 → 截止时间戳

        std::atomic<bool> stopped_{false};
        std::atomic<bool> paused_{false};
        std::atomic<bool> busy_{false};
        std::atomic<bool> cancel_{false};

        // 监视触发去抖：短时间内多次变化只触发一次迁移
        std::atomic<int64_t> lastWatcherTrigger_{0};

        std::vector<std::unique_ptr<DirectoryWatcher>> watchers_;
        std::unique_ptr<PipeServer> pipe_;

        std::mutex lastRunMtx_;
        std::wstring lastRunInfo_ = L"未运行";

        std::thread migrateThread_;
        std::mutex migrateThreadMtx_;

        std::time_t nextRun_ = 0; // 下一次定时运行时刻
    };

} // namespace dirmv
