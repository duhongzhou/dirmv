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
        LOGI(L"[控制] 收到停止请求，正在中止当前任务并收尾");
        stopped_ = true;
        cancel_ = true;
    }
    void ServiceWorker::RequestPause()
    {
        LOGI(L"[控制] 服务已暂停：不再开始新的迁移，正在进行的子目录会先完成");
        paused_ = true;
    }
    void ServiceWorker::RequestResume()
    {
        LOGI(L"[控制] 服务已恢复：重新接受监视/定时触发");
        paused_ = false;
    }

    // ---------------------------------------------------------------------------
    // 失败冷却
    // ---------------------------------------------------------------------------
    bool ServiceWorker::CoolingDown(const std::wstring &srcPath)
    {
        std::lock_guard<std::mutex> lk(cooldownMtx_);
        auto it = cooldownUntil_.find(srcPath);
        if (it == cooldownUntil_.end())
            return false;
        if (it->second <= (int64_t)time(nullptr))
        {
            cooldownUntil_.erase(it);
            return false;
        }
        return true;
    }

    void ServiceWorker::CoolDown(const std::wstring &srcPath, int seconds)
    {
        if (seconds <= 0)
            return;
        std::lock_guard<std::mutex> lk(cooldownMtx_);
        cooldownUntil_[srcPath] = (int64_t)time(nullptr) + seconds;
        // 顺手清理已过期项，避免长期运行时无限增长
        for (auto it = cooldownUntil_.begin(); it != cooldownUntil_.end();)
        {
            if (it->second <= (int64_t)time(nullptr))
                it = cooldownUntil_.erase(it);
            else
                ++it;
        }
    }

    void ServiceWorker::ClearCooldowns()
    {
        std::lock_guard<std::mutex> lk(cooldownMtx_);
        cooldownUntil_.clear();
    }

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

        // 0 分钟 = 不自动重试
        if (delay <= 0)
        {
            LOGE(L"[占用] 未启用自动重试（RetryDelayMinutes=0）");
            return;
        }

        // 创建一次性重试计划任务（只创建一次）
        std::wstring exePath = Path::ExePath();
        std::wstring cfgPath = ConfigStore::DefaultPath();
        std::wstring taskName = L"dirMover_RetryOnce";
        std::wstring err = Scheduler::CreateOnceRetryTask(taskName, exePath, cfgPath, delay);
        if (!err.empty())
            LOGE(L"创建重试任务失败: " + err);
    }

    // ---------------------------------------------------------------------------
    void ServiceWorker::TriggerMigrate(bool async, const std::wstring &onlyWatchDir,
                                       bool manual)
    {
        if (busy_.exchange(true))
        {
            LOGW(L"已有迁移任务在运行，忽略触发");
            return;
        }
        cancel_ = false;
        if (manual)
            ClearCooldowns();

        auto job = [this, onlyWatchDir]()
        {
            Config local;
            {
                std::lock_guard<std::mutex> lk(cfgMtx_);
                local = cfg_;
            }

            MigrateEngine engine(local);

            // 失败冷却：服务进程内记录，避免监视风暴下反复重试同一目录
            SkipFn skip = [this](const std::wstring &src)
            { return CoolingDown(src); };

            auto recordCooldown = [this, &local](const MigrateResult &r)
            {
                int retryMin = local.retryDelayMinutes > 0 ? local.retryDelayMinutes : 5;
                for (auto &st : r.subtasks)
                {
                    if (st.state == SubtaskState::Locked)
                        CoolDown(st.src, (retryMin < 1 ? 1 : retryMin) * 60);
                    else if (st.state == SubtaskState::Failed)
                        CoolDown(st.src, 1800); // 失败目录 30 分钟内不自动重试
                }
            };

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
                                                 { OnLockNotice(n); },
                                                 skip);
                recordCooldown(result);
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
                { OnLockNotice(n); },
                skip);
            recordCooldown(result);

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
            // 人工触发的迁移不受失败冷却限制，立即重试。
            TriggerMigrate(true, Path::Trim(arg), true);
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
            LOGI(L"[控制] 收到管道『停止当前任务』请求");
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
                         if (paused_ || busy_)
                             return;
                         // 去抖：目录活动频繁时（如应用持续写日志）限制触发频率，
                         // 避免「触发→迁移→再触发」的循环。
                         int64_t now = (int64_t)time(nullptr);
                         int64_t last = lastWatcherTrigger_.load();
                         if (now - last < 5)
                             return;
                         lastWatcherTrigger_ = now;
                         LOGI(L"监视触发迁移");
                         TriggerMigrate(true, L"", false);
                     });
            watchers_.push_back(std::move(w));
            LOGI(L"[监视] 已监视目录: " + r.watchDir);
        }
    }

    void ServiceWorker::Run(std::function<void()> onStopWait)
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

        // 启动自愈：上次迁移中途崩溃/断电会在源目录旁留下 .dirmv-moving /
        // .dirmv-old。必须在启动监视器之前处理，避免残留目录被当作新目录。
        {
            Config recoverCfg;
            {
                std::lock_guard<std::mutex> lk(cfgMtx_);
                recoverCfg = cfg_;
            }
            MigrateEngine eng(recoverCfg);
            for (auto &r : recoverCfg.rules)
            {
                if (!r.enabled)
                    continue;
                auto rep = eng.RecoverRule(r);
                if (rep.any())
                    LOGI(L"[恢复] 规则 [" + r.watchDir + L"] 发现残留 " +
                         std::to_wstring(rep.scanned) + L" 个：补建链接 " +
                         std::to_wstring(rep.relinked) + L"，还原原名 " +
                         std::to_wstring(rep.restored) + L"，清理重复 " +
                         std::to_wstring(rep.cleaned) + L"，保留待查 " +
                         std::to_wstring(rep.kept));
            }
        }

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
                        TriggerMigrate(true, L"", false);
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
        // 等待正在进行的迁移安全收尾：取消会在下一个文件/进度回调处生效，
        // 源数据保持完整。期间每 200ms 上报一次 STOP_PENDING 检查点，
        // 避免 SCM 因 30 秒内没有进展而强制终止（事件 7034）。
        while (busy_.load())
        {
            if (onStopWait)
                onStopWait();
            Sleep(200);
        }
        if (migrateThread_.joinable())
            migrateThread_.join();
    }

} // namespace dirmv
