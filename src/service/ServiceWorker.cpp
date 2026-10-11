#include "service/ServiceWorker.h"
#include "core/PathUtil.h"
#include "core/Log.h"
#include "core/Scheduler.h"

#include <windows.h>
#include <sstream>
#include <shellapi.h>

namespace dirmv
{

    ServiceWorker::ServiceWorker()
    {
        ReloadConfig();
    }

    ServiceWorker::~ServiceWorker()
    {
        RequestStop();
        if (migrateThread_.joinable())
            migrateThread_.join();
    }

    void ServiceWorker::ReloadConfig()
    {
        Config c;
        if (!ConfigStore::Load(ConfigStore::DefaultPath(), c))
            c.ApplyDefaults();
        std::lock_guard<std::mutex> lk(cfgMtx_);
        cfg_ = c;
        nextRun_ = 0; // 触发重新计算
    }

    void ServiceWorker::RequestStop()
    {
        stopped_ = true;
        cancel_ = true;
    }
    void ServiceWorker::RequestPause() { paused_ = true; }
    void ServiceWorker::RequestResume() { paused_ = false; }

    // ---------------------------------------------------------------------------
    void ServiceWorker::OnLockNotice(const LockNotice &notice)
    {
        // 汇总占用信息
        std::wstring detail = L"子任务 [" + notice.subtask + L"] 的文件被占用：\r\n";
        int shown = 0;
        for (auto &l : notice.locks)
        {
            if (shown++ >= 5)
            {
                detail += L"  ...\r\n";
                break;
            }
            detail += L"  • " + Path::FileName(l.filePath);
            if (!l.processes.empty())
            {
                detail += L"  ← ";
                for (size_t i = 0; i < l.processes.size(); ++i)
                {
                    if (i)
                        detail += L", ";
                    detail += l.processes[i];
                }
            }
            detail += L"\r\n";
        }

        int delay = 5;
        {
            std::lock_guard<std::mutex> lk(cfgMtx_);
            delay = cfg_.retryDelayMinutes;
        }
        detail += L"\r\n将创建一次性重试任务，在 " + std::to_wstring(delay) +
                  L" 分钟后自动重试（仅一次）。\r\n";
        detail += L"处理建议：请保存并关闭上述程序后等待自动重试，或手动执行迁移。";

        LOGE(L"[占用通知] " + detail);

        // 服务模式下弹一个消息框到交互桌面（尽力而为）+ 托盘通知
        // 注意：服务在 Session 0，弹窗可能不可见；主要依靠日志与重试任务。
        if (notice.locks.empty())
            return;

        // 创建一次性重试计划任务（只创建一次）
        std::wstring exePath = Path::ExePath();
        std::wstring cfgPath = ConfigStore::DefaultPath();
        std::wstring taskName = L"dirMover_RetryOnce";
        std::wstring err = Scheduler::CreateOnceRetryTask(taskName, exePath, cfgPath, delay);
        if (!err.empty())
            LOGE(L"创建重试任务失败: " + err);
    }

    // ---------------------------------------------------------------------------
    void ServiceWorker::TriggerMigrate(bool async, const std::wstring &onlyWatchDir)
    {
        if (busy_.exchange(true))
        {
            LOGW(L"已有迁移任务在运行，忽略触发");
            return;
        }
        cancel_ = false;

        auto job = [this, onlyWatchDir]()
        {
            Config local;
            {
                std::lock_guard<std::mutex> lk(cfgMtx_);
                local = cfg_;
            }

            MigrateEngine engine(local);

            // 指定了某条规则的监视目录：只迁移这一条（供 GUI 触发使用）。
            if (!onlyWatchDir.empty())
            {
                const Rule *target = nullptr;
                for (auto &r : local.rules)
                    if (Path::Equals(r.watchDir, onlyWatchDir))
                    {
                        target = &r;
                        break;
                    }
                if (!target)
                {
                    LOGW(L"未找到指定规则，跳过迁移: " + onlyWatchDir);
                    std::lock_guard<std::mutex> lk(lastRunMtx_);
                    lastRunInfo_ = L"未找到指定规则";
                    busy_ = false;
                    return;
                }
                std::wstring verr = local.ValidateOneRule(*target);
                if (!verr.empty())
                {
                    LOGW(L"指定规则不可用，跳过迁移: " + verr);
                    std::lock_guard<std::mutex> lk(lastRunMtx_);
                    lastRunInfo_ = L"指定规则不可用: " + verr;
                    busy_ = false;
                    return;
                }
                auto result = engine.MigrateRule(*target,
                                                 nullptr, &cancel_,
                                                 [this](const LockNotice &n)
                                                 { OnLockNotice(n); });
                {
                    std::lock_guard<std::mutex> lk(lastRunMtx_);
                    lastRunInfo_ = L"迁移完成(单条): 总 " + std::to_wstring(result.total) +
                                   L"，成功 " + std::to_wstring(result.migrated) +
                                   L"，跳过 " + std::to_wstring(result.skipped) +
                                   L"，占用 " + std::to_wstring(result.locked) +
                                   L"，失败 " + std::to_wstring(result.failed);
                }
                busy_ = false;
                return;
            }

            // 自动/定时迁移：迁移「全部正确可用」的规则（无效规则由引擎逐条跳过），
            // 因此这里不做整份配置校验，避免一条无效规则阻断整体。
            bool anyUsable = false;
            for (auto &r : local.rules)
            {
                if (r.enabled && local.ValidateOneRule(r).empty())
                {
                    anyUsable = true;
                    break;
                }
            }
            if (!anyUsable)
            {
                LOGW(L"没有可用的规则，跳过本次自动/定时迁移");
                std::lock_guard<std::mutex> lk(lastRunMtx_);
                lastRunInfo_ = L"没有可用的规则";
                busy_ = false;
                return;
            }

            auto result = engine.Migrate(
                nullptr, &cancel_,
                [this](const LockNotice &n)
                { OnLockNotice(n); });

            {
                std::lock_guard<std::mutex> lk(lastRunMtx_);
                lastRunInfo_ = L"迁移完成: 总 " + std::to_wstring(result.total) +
                               L"，成功 " + std::to_wstring(result.migrated) +
                               L"，跳过 " + std::to_wstring(result.skipped) +
                               L"，占用 " + std::to_wstring(result.locked) +
                               L"，失败 " + std::to_wstring(result.failed);
            }
            busy_ = false;
        };

        if (async)
        {
            std::lock_guard<std::mutex> lk(migrateThreadMtx_);
            if (migrateThread_.joinable())
                migrateThread_.join();
            migrateThread_ = std::thread(job);
        }
        else
        {
            job();
        }
    }

    // ---------------------------------------------------------------------------
    std::wstring ServiceWorker::HandlePipe(PipeCommand cmd, const std::wstring &arg)
    {
        switch (cmd)
        {
        case PipeCommand::Status:
        {
            std::lock_guard<std::mutex> lk(lastRunMtx_);
            std::wstring state = busy_ ? L"running" : (paused_ ? L"paused" : L"idle");
            return L"{\"ok\":true,\"state\":\"" + state +
                   L"\",\"last\":\"" + lastRunInfo_ + L"\"}";
        }
        case PipeCommand::Migrate:
            // arg 为要迁移的规则「监视目录」；为空表示迁移全部可用规则（定时/自动）
            TriggerMigrate(true, Path::Trim(arg));
            return L"{\"ok\":true,\"msg\":\"迁移已开始\"}";
        case PipeCommand::Restore:
        {
            Config local;
            {
                std::lock_guard<std::mutex> lk(cfgMtx_);
                local = cfg_;
            }
            MigrateEngine engine(local);
            auto r = engine.Restore(nullptr, nullptr);
            return L"{\"ok\":true,\"msg\":\"恢复完成: 成功 " +
                   std::to_wstring(r.migrated) + L" 失败 " +
                   std::to_wstring(r.failed) + L"\"}";
        }
        case PipeCommand::Stop:
            cancel_ = true;
            return L"{\"ok\":true,\"msg\":\"已请求停止当前任务\"}";
        case PipeCommand::Reload:
            ReloadConfig();
            SetupWatchers(); // 配置变化后重建监视器
            return L"{\"ok\":true,\"msg\":\"配置已重新加载\"}";
        default:
            return L"{\"ok\":false,\"msg\":\"未知命令\"}";
        }
    }

    // ---------------------------------------------------------------------------
    // ---------------------------------------------------------------------------
    // 依据当前配置重建监视器（仅「正确可用的启用规则」，并按监视目录去重）
    // ---------------------------------------------------------------------------
    void ServiceWorker::SetupWatchers()
    {
        // 先停止并清空旧监视器
        for (auto &w : watchers_)
            if (w)
                w->Stop();
        watchers_.clear();

        Config local;
        {
            std::lock_guard<std::mutex> lk(cfgMtx_);
            local = cfg_;
        }
        if (!local.enableWatcher)
            return;

        std::vector<std::wstring> watched;
        for (auto &r : local.rules)
        {
            if (!r.enabled)
                continue;
            // 只监视「正确可用」的规则
            if (!local.ValidateOneRule(r).empty())
            {
                LOGW(L"[监视] 跳过不可用规则: " + r.watchDir);
                continue;
            }
            std::wstring norm = Path::Normalize(r.watchDir);
            bool dup = false;
            for (auto &d : watched)
                if (Path::Equals(d, norm))
                {
                    dup = true;
                    break;
                }
            if (dup)
                continue;
            watched.push_back(norm);

            auto w = std::make_unique<DirectoryWatcher>();
            w->Start(r.watchDir, [this](const std::wstring & /*full*/)
                     {
                         if (!paused_ && !busy_)
                         {
                             LOGI(L"监视触发迁移");
                             TriggerMigrate(true);
                         } });
            watchers_.push_back(std::move(w));
            LOGI(L"[监视] 已监视目录: " + r.watchDir);
        }
    }

    void ServiceWorker::Run()
    {
        ReloadConfig();

        Config local;
        {
            std::lock_guard<std::mutex> lk(cfgMtx_);
            local = cfg_;
        }
        LOGI(L"服务工作线程启动");

        // 命名管道控制通道
        pipe_ = std::make_unique<PipeServer>();
        pipe_->Start(PipeServer::DefaultPipeName(),
                     [this](PipeCommand c, const std::wstring &a)
                     { return HandlePipe(c, a); });

        // 每条「正确可用的启用规则」一个文件系统监视器
        SetupWatchers();

        // 计算下一次定时运行

        // 计算下一次定时运行
        {
            std::lock_guard<std::mutex> lk(cfgMtx_);
            NextRun nr = Scheduler::ComputeNextRun(cfg_.schedule, time(nullptr));
            nextRun_ = nr.valid ? nr.when : 0;
            if (nextRun_)
            {
                std::tm lt{};
                localtime_s(&lt, &nextRun_);
                LOGI(L"下次定时迁移: " + std::to_wstring(lt.tm_year + 1900) + L"-" +
                     std::to_wstring(lt.tm_mon + 1) + L"-" + std::to_wstring(lt.tm_mday) +
                     L" " + std::to_wstring(lt.tm_hour) + L":" + std::to_wstring(lt.tm_min));
            }
        }

        auto lastCheck = std::chrono::steady_clock::now();

        while (!stopped_.load())
        {
            Sleep(500);
            if (paused_.load())
                continue;

            // 每秒检查是否到达定时点
            auto nowT = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(nowT - lastCheck).count() >= 1)
            {
                lastCheck = nowT;
                std::time_t cur = time(nullptr);
                std::time_t nr = 0;
                {
                    std::lock_guard<std::mutex> lk(cfgMtx_);
                    nr = nextRun_;
                }
                if (nr != 0 && cur >= nr)
                {
                    LOGI(L"到达定时点，触发迁移");
                    if (!busy_)
                        TriggerMigrate(true);
                    // 重新计算下一次
                    std::lock_guard<std::mutex> lk(cfgMtx_);
                    NextRun nx = Scheduler::ComputeNextRun(cfg_.schedule, time(nullptr));
                    nextRun_ = nx.valid ? nx.when : 0;
                }
            }
        }

        for (auto &w : watchers_)
            if (w)
                w->Stop();
        if (pipe_)
            pipe_->Stop();
        cancel_ = true;
        if (migrateThread_.joinable())
            migrateThread_.join();
    }

} // namespace dirmv
