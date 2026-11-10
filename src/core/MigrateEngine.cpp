#include "core/MigrateEngine.h"
#include "core/PathUtil.h"
#include "core/Config.h"
#include "core/Log.h"

#include <windows.h>
#include <thread>
#include <deque>
#include <algorithm>

namespace dirmv
{

    MigrateEngine::MigrateEngine(const Config &cfg) : cfg_(cfg) {}

    std::wstring MigrateEngine::LogPathFor(const std::wstring &targetDir) const
    {
        return Path::Combine(targetDir, L"link_restore_log.txt");
    }

    namespace
    {
        // 迁移过程中的临时目录后缀：源目录被原子改名到这里，然后创建链接，
        // 最后才删除旧数据。任何一步失败都可以安全还原。
        constexpr const wchar_t *kMovingSuffix = L".dirmv-moving";
        constexpr const wchar_t *kOldSuffix = L".dirmv-old";

        bool HasTempSuffix(const std::wstring &name)
        {
            auto endsWith = [&](const wchar_t *s)
            {
                size_t n = wcslen(s);
                return name.size() >= n &&
                       _wcsicmp(name.c_str() + (name.size() - n), s) == 0;
            };
            return endsWith(kMovingSuffix) || endsWith(kOldSuffix);
        }

        std::wstring ErrText(const std::wstring &msg, DWORD e)
        {
            return msg + L"(错误 " + std::to_wstring(e) + L")";
        }

        // 解析 <base>.dirmv-moving[.N] / <base>.dirmv-old[.N]
        bool SplitTempName(const std::wstring &name, std::wstring &base,
                           bool &isMoving, std::wstring &index)
        {
            auto match = [&](const std::wstring &suf, bool moving) -> bool
            {
                size_t p = name.rfind(suf);
                if (p == std::wstring::npos || p == 0)
                    return false;
                std::wstring rest = name.substr(p + suf.size());
                if (!rest.empty())
                {
                    if (rest[0] != L'.' || rest.size() < 2)
                        return false;
                    for (size_t i = 1; i < rest.size(); ++i)
                        if (rest[i] < L'0' || rest[i] > L'9')
                            return false;
                }
                base = name.substr(0, p);
                index = rest;
                isMoving = moving;
                return true;
            };
            return match(kMovingSuffix, true) || match(kOldSuffix, false);
        }
    } // namespace

    std::vector<std::wstring> MigrateEngine::ScanPending(const Rule &rule) const
    {
        std::vector<std::wstring> pending;
        auto subdirs = Path::ListSubDirectories(rule.watchDir);
        for (auto &name : subdirs)
        {
            if (cfg_.IsExcluded(name, rule))
                continue;
            // 自身迁移过程中的临时目录不能再被当作迁移对象
            if (HasTempSuffix(name))
                continue;
            std::wstring src = Path::Combine(rule.watchDir, name);
            if (Path::IsReparsePoint(src))
                continue;
            std::wstring dst = Path::Combine(rule.targetDir, name);
            if (cfg_.skipExisting && Path::DirectoryExists(dst))
                continue;
            pending.push_back(name);
        }
        return pending;
    }

    // ---------------------------------------------------------------------------
    // 崩溃残留恢复（.dirmv-moving / .dirmv-old）
    //   原则：任何情况下都不删除唯一副本；不确定就保留并报告。
    // ---------------------------------------------------------------------------
    MigrateEngine::RecoverReport MigrateEngine::RecoverRule(const Rule &rule, bool dryRun,
                                                            std::atomic<bool> *cancel)
    {
        RecoverReport rep;
        rep.dryRun = dryRun;
        auto names = Path::ListSubDirectories(rule.watchDir);

        auto addItem = [&](const std::wstring &path, int kind, const std::wstring &action)
        { rep.items.push_back(RecoverItem{path, action, kind}); };

        for (auto &name : names)
        {
            if (cancel && cancel->load())
                break;
            std::wstring base, index;
            bool isMoving = false;
            if (!SplitTempName(name, base, isMoving, index))
                continue;
            rep.scanned++;

            std::wstring residual = Path::Combine(rule.watchDir, name);
            std::wstring src = Path::Combine(rule.watchDir, base);
            std::wstring dst = Path::Combine(rule.targetDir, base);

            // 白名单目录绝不复活（用户明确要求不要动它）
            if (cfg_.IsExcluded(base, rule))
            {
                rep.kept++;
                addItem(residual, 4, L"目录在白名单中，保留不动");
                LOGW(L"[恢复] 残留属于白名单目录，保留: " + residual);
                continue;
            }

            bool srcIsLink = Path::IsReparsePoint(src);
            bool srcExists = Path::DirectoryExists(src); // 跟随链接（悬空链接为 false）
            bool srcIsRealDir = srcExists && !srcIsLink;
            bool dstExists = Path::DirectoryExists(dst);

            // 源名已被「指向目标」的链接接手 → 数据完整，残留是重复数据
            bool linkReady = false;
            if (srcExists && srcIsLink)
            {
                std::wstring t = Path::ReadLinkTarget(src);
                linkReady = !t.empty() &&
                            Path::Equals(Path::Normalize(t), Path::Normalize(dst));
            }

            auto keep = [&](const std::wstring &why)
            {
                rep.kept++;
                addItem(residual, 4, why);
                LOGW(L"[恢复] 保留残留目录: " + residual + L"（" + why + L"）");
            };

            auto cleanResidual = [&](const std::wstring &why) -> bool
            {
                if (cfg_.lockDetectEnabled)
                {
                    auto locks = FileEngine::DetectLocks(residual);
                    if (!locks.empty())
                    {
                        keep(L"仍有 " + std::to_wstring(locks.size()) + L" 个文件被占用");
                        return false;
                    }
                }
                if (dryRun)
                {
                    rep.cleaned++;
                    addItem(residual, 3, L"将清理（" + why + L"）");
                    LOGI(L"[恢复] 将清理重复数据: " + residual + L"（" + why + L"）");
                    return true;
                }
                std::wstring err;
                if (!FileEngine::RemoveTree(residual, &err))
                {
                    keep(L"删除失败: " + err);
                    return false;
                }
                rep.cleaned++;
                addItem(residual, 3, L"已清理（" + why + L"）");
                LOGI(L"[恢复] 已清理重复数据: " + residual + L"（" + why + L"）");
                return true;
            };

            auto restoreName = [&]() -> bool
            {
                if (dryRun)
                {
                    rep.restored++;
                    addItem(residual, 2, L"将改回原名（目标无数据，恢复原状）");
                    LOGI(L"[恢复] 将把 " + residual + L" 改回 " + src);
                    return true;
                }
                if (!MoveFileExW(residual.c_str(), src.c_str(), 0))
                {
                    keep(L"改回原名失败" + ErrText(L"", GetLastError()));
                    return false;
                }
                rep.restored++;
                addItem(residual, 2, L"已改回原名（目标无数据，恢复原状）");
                LOGI(L"[恢复] 已把 " + residual + L" 改回 " + src);
                return true;
            };

            auto relink = [&]() -> bool
            {
                // 悬空链接先移除，否则 CreateLink 报 183
                if (!dryRun && !srcExists && Path::IsReparsePoint(src))
                    FileEngine::RemoveLink(src, nullptr);
                if (dryRun)
                {
                    rep.relinked++;
                    addItem(residual, 1, L"将补建链接（目标已有完整数据）");
                    LOGI(L"[恢复] 将为 " + src + L" 补建指向 " + dst + L" 的链接");
                    return true;
                }
                std::wstring err;
                if (!FileEngine::CreateLink(src, dst,
                                            cfg_.linkType == LinkType::Junction, &err))
                {
                    keep(L"补建链接失败: " + err);
                    return false;
                }
                rep.relinked++;
                addItem(residual, 1, L"已补建链接（目标已有完整数据）");
                LOGI(L"[恢复] 已为 " + src + L" 补建指向 " + dst + L" 的链接");
                return true;
            };

            if (linkReady)
            {
                cleanResidual(L"链接与目标数据均已就绪，残留为重复数据");
                continue;
            }
            if (!srcExists && !srcIsLink && dstExists)
            {
                if (relink() && !dryRun)
                    cleanResidual(L"补建链接后清理旧数据");
                continue;
            }
            if (!srcExists && !srcIsLink)
            {
                restoreName();
                continue;
            }
            if (srcIsRealDir)
            {
                // 源目录仍真实存在：上次迁移失败已把数据还原，残留是重复副本。
                // 但目标里也可能有更早一次迁移的完整副本，保留待人工确认。
                keep(L"源目录仍然存在，残留为重复副本，保留待人工确认");
                continue;
            }
            keep(L"无法判断安全处理方式，保留待人工确认");
        }
        return rep;
    }

    // ---------------------------------------------------------------------------
    // 日志
    // ---------------------------------------------------------------------------
    void MigrateEngine::WriteLogHeader(const std::wstring &targetDir)
    {
        std::wstring text;
        text += LOG_HEADER;
        text += L"\r\n";
        text += L"DATE: ";
        text += std::to_wstring((long long)time(nullptr));
        text += L"\r\n";
        text += L"TARGET: ";
        text += targetDir;
        text += L"\r\n\r\n";
        Path::WriteFileUtf8(LogPathFor(targetDir), text);
    }

    void MigrateEngine::AppendLinkLog(const std::wstring &targetDir,
                                      const std::wstring &src, const std::wstring &dst)
    {
        std::wstring line = L"LINK|" + src + L"|" + dst + L"\r\n";
        Path::AppendFileUtf8(LogPathFor(targetDir), line);
    }

    std::vector<MigrateEngine::LinkRec> MigrateEngine::ReadLinkLog(
        const std::wstring &targetDir) const
    {
        std::vector<LinkRec> recs;
        std::wstring content = Path::ReadFileUtf8(LogPathFor(targetDir));
        if (content.empty())
            return recs;
        if (!content.empty() && content[0] == 0xFEFF)
            content.erase(0, 1);

        size_t pos = 0;
        while (pos < content.size())
        {
            size_t eol = content.find(L'\n', pos);
            std::wstring line = (eol == std::wstring::npos)
                                    ? content.substr(pos)
                                    : content.substr(pos, eol - pos);
            pos = (eol == std::wstring::npos) ? content.size() : eol + 1;
            line = Path::Trim(line);
            if (line.rfind(L"LINK|", 0) != 0)
                continue;

            std::wstring rest = line.substr(5);
            size_t sep = rest.find(L'|');
            if (sep == std::wstring::npos)
                continue;
            LinkRec r;
            r.src = Path::Trim(rest.substr(0, sep));
            r.dst = Path::Trim(rest.substr(sep + 1));
            r.targetDir = targetDir;
            if (!r.src.empty() && !r.dst.empty())
            {
                bool dup = false;
                for (auto &e : recs)
                    if (Path::Equals(e.src, r.src))
                    {
                        e = r;
                        dup = true;
                        break;
                    }
                if (!dup)
                    recs.push_back(r);
            }
        }
        return recs;
    }

    // ---------------------------------------------------------------------------
    // 单个子任务迁移（安全顺序：复制 → 校验 → 原子改名 → 建链接 → 删旧数据）
    //   不变量：任何失败路径都至少保留一份完整数据；绝不删除唯一副本。
    // ---------------------------------------------------------------------------
    void MigrateEngine::MigrateSubtask(const Rule &rule, const std::wstring &name,
                                       SubtaskInfo &info, ProgressFn cb,
                                       std::atomic<bool> *cancel,
                                       ProgressListener onProgress)
    {
        (void)onProgress;
        std::wstring src = Path::Combine(rule.watchDir, name);
        std::wstring dst = Path::Combine(rule.targetDir, name);
        std::wstring moving = src + kMovingSuffix;
        std::wstring old = src + kOldSuffix;
        info.name = name;
        info.src = src;
        info.dst = dst;

        auto finish = [&](SubtaskState st, const std::wstring &msg)
        {
            info.state = st;
            info.message = msg;
            if (!msg.empty())
                LOGW(L"[" + name + L"] " + msg);
        };

        DirStat st;
        FileEngine::Scan(src, st, cancel);
        info.bytes = st.totalBytes;

        // 1) 目标已存在：绝不覆盖
        if (Path::DirectoryExists(dst))
        {
            finish(SubtaskState::Skipped, L"目标已存在，跳过（避免覆盖）");
            return;
        }

        // 1.5) 目标磁盘剩余空间预检：不足则直接失败，不产生半成品目标
        {
            wchar_t volRoot[MAX_PATH]{};
            if (GetVolumePathNameW(rule.targetDir.c_str(), volRoot, MAX_PATH))
            {
                ULARGE_INTEGER avail{};
                if (GetDiskFreeSpaceExW(volRoot, &avail, nullptr, nullptr))
                {
                    constexpr uint64_t kReserve = 64ull * 1024 * 1024; // 64 MB 余量
                    if (st.totalBytes + kReserve > avail.QuadPart)
                    {
                        finish(SubtaskState::Failed,
                               L"目标磁盘空间不足：需要 " + Path::FormatBytes(st.totalBytes) +
                                   L"，可用 " + Path::FormatBytes(avail.QuadPart));
                        return;
                    }
                }
            }
        }

        // 2) 迁移前占用检测
        if (cfg_.lockDetectEnabled)
        {
            auto locks = FileEngine::DetectLocks(src);
            if (!locks.empty())
            {
                info.state = SubtaskState::Locked;
                info.locks = std::move(locks);
                info.message = L"检测到 " + std::to_wstring(info.locks.size()) +
                               L" 个被占用的文件，本次不迁移";
                LOGW(L"[占用] " + name + L" : " + info.message);
                return;
            }
        }

        // 3) 复制到目标（源保持原样；失败只需清掉半成品目标）
        std::wstring err;
        int failures = FileEngine::CopyTree(src, dst, cb, cancel, &err);
        if (failures != 0 || (cancel && cancel->load()))
        {
            FileEngine::RemoveTree(dst, nullptr); // 源未动，安全
            finish(SubtaskState::Failed, err.empty() ? L"复制出错，已回滚" : err);
            return;
        }

        // 4) 校验复制完整性（文件数 + 字节数）
        DirStat dstat;
        FileEngine::Scan(dst, dstat, nullptr);
        if (dstat.fileCount != st.fileCount || dstat.totalBytes != st.totalBytes)
        {
            FileEngine::RemoveTree(dst, nullptr);
            finish(SubtaskState::Failed, L"复制校验失败(文件数/大小不一致)，已回滚");
            return;
        }

        // 5) 复制期间目录可能被程序打开：再检测一次，绝不带着打开的文件迁移
        if (cfg_.lockDetectEnabled)
        {
            auto locks = FileEngine::DetectLocks(src);
            if (!locks.empty())
            {
                FileEngine::RemoveTree(dst, nullptr); // 源未动，安全
                info.state = SubtaskState::Locked;
                info.locks = std::move(locks);
                info.message = L"复制期间检测到 " + std::to_wstring(info.locks.size()) +
                               L" 个文件被占用，已放弃本次迁移（未删任何源数据）";
                LOGW(L"[占用] " + name + L" : " + info.message);
                return;
            }
        }

        // 6) 原子改名：源 → 临时目录（链接名腾空，且可随时改回）。
        //    若之前遗留了同名临时目录，改名保留而不是删除，避免误删数据。
        if (Path::DirectoryExists(moving))
        {
            std::wstring keep = old;
            for (int i = 1; Path::DirectoryExists(keep) && i < 100; ++i)
                keep = old + L"." + std::to_wstring(i);
            if (!MoveFileExW(moving.c_str(), keep.c_str(), 0))
            {
                finish(SubtaskState::Failed, L"存在残留临时目录且无法移动: " + moving);
                return;
            }
            LOGW(L"[清理] 发现上次残留的临时目录，已保留为: " + keep);
        }
        if (!MoveFileExW(src.c_str(), moving.c_str(), 0))
        {
            DWORD e = GetLastError();
            FileEngine::RemoveTree(dst, nullptr); // 源未动，安全
            finish(SubtaskState::Failed,
                   L"源目录被占用，无法重命名，已回滚" + ErrText(L"", e));
            return;
        }

        // 7) 建链接
        bool isJunction = (cfg_.linkType == LinkType::Junction);
        std::wstring lerr;
        if (!FileEngine::CreateLink(src, dst, isJunction, &lerr))
        {
            // 还原：临时目录改回原名；此刻源数据完好无损
            if (Path::DirectoryExists(src) && FileEngine::IsEmptyDir(src))
                RemoveDirectoryW(src.c_str());
            if (MoveFileExW(moving.c_str(), src.c_str(), 0))
            {
                FileEngine::RemoveTree(dst, nullptr);
            }
            else
            {
                LOGW(L"[严重] 源目录未能还原，数据同时保留在: " + moving + L" 与 " + dst);
            }
            finish(SubtaskState::Failed, lerr);
            return;
        }

        // 8) 链接已就绪，删除旧数据（临时目录）。
        //    若仍有文件被占用，绝不强行删除，改名保留为 .dirmv-old 并告警。
        auto keepLeftover = [&]()
        {
            std::wstring keep = old;
            for (int i = 1; Path::DirectoryExists(keep) && i < 100; ++i)
                keep = old + L"." + std::to_wstring(i);
            if (!MoveFileExW(moving.c_str(), keep.c_str(), 0))
                keep = moving;
            LOGW(L"[清理] 旧数据未能完全删除，已保留为: " + keep +
                 L"（数据已完整迁移到 " + dst + L"）");
        };
        if (cfg_.lockDetectEnabled && !FileEngine::DetectLocks(moving).empty())
        {
            keepLeftover();
        }
        else
        {
            std::wstring derr;
            if (!FileEngine::RemoveTree(moving, &derr))
            {
                LOGW(L"[清理] " + derr);
                keepLeftover();
            }
        }

        info.state = SubtaskState::Migrated;
        AppendLinkLog(rule.targetDir, src, dst);
        LOGI(L"[完成] " + name);
    }

    // ---------------------------------------------------------------------------
    // 单条规则迁移
    // ---------------------------------------------------------------------------
    MigrateResult MigrateEngine::MigrateRule(const Rule &rule,
                                             ProgressListener onProgress,
                                             std::atomic<bool> *cancel,
                                             LockListener onLock,
                                             SkipFn skip)
    {
        MigrateResult result;

        auto names = ScanPending(rule);
        if (skip && !names.empty())
        {
            std::vector<std::wstring> kept;
            kept.reserve(names.size());
            for (auto &n : names)
            {
                std::wstring full = Path::Combine(rule.watchDir, n);
                if (skip(full))
                {
                    LOGI(L"[冷却] 跳过（近期失败，稍后重试）: " + n);
                    continue;
                }
                kept.push_back(n);
            }
            names.swap(kept);
        }
        result.total = (int)names.size();
        if (names.empty())
        {
            LOGI(L"规则 [" + rule.watchDir + L"] 没有需要迁移的目录");
            return result;
        }

        WriteLogHeader(rule.targetDir);

        // 1) 扫描大小用于 ETA
        uint64_t grandTotal = 0;
        std::vector<DirStat> stats(names.size());
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (cancel && cancel->load())
                return result;
            std::wstring src = Path::Combine(rule.watchDir, names[i]);
            FileEngine::Scan(src, stats[i], cancel);
            grandTotal += stats[i].totalBytes;
            if (onProgress)
            {
                ProgressInfo pi;
                pi.totalItems = result.total;
                pi.doneItems = (int)i;
                pi.currentItem = names[i];
                pi.phase = L"扫描中";
                onProgress(pi);
            }
        }

        // 2) 子任务独立执行（每个顶层目录一个子任务，互不影响）
        result.subtasks.resize(names.size());

        std::deque<size_t> queue;
        for (size_t i = 0; i < names.size(); ++i)
            queue.push_back(i);

        std::mutex qmtx, pmtx;
        uint64_t sharedDone = 0;
        int doneItems = 0;
        auto startTime = std::chrono::steady_clock::now();

        auto worker = [&]()
        {
            for (;;)
            {
                size_t idx;
                {
                    std::lock_guard<std::mutex> lk(qmtx);
                    if (queue.empty())
                        break;
                    idx = queue.front();
                    queue.pop_front();
                }
                if (cancel && cancel->load())
                    break;

                SubtaskInfo &info = result.subtasks[idx];
                uint64_t itemBase = 0;
                for (size_t k = 0; k < idx; ++k)
                    itemBase += stats[k].totalBytes;

                ProgressFn cb = nullptr;
                if (onProgress)
                {
                    cb = [&](uint64_t doneBytes, uint64_t, const std::wstring &cur)
                    {
                        std::lock_guard<std::mutex> lk(pmtx);
                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - startTime).count();
                        ProgressInfo pi;
                        pi.totalItems = result.total;
                        pi.doneItems = doneItems;
                        pi.totalBytes = grandTotal;
                        pi.doneBytes = sharedDone + doneBytes;
                        pi.currentItem = cur;
                        pi.phase = L"迁移中";
                        pi.percent = grandTotal
                                         ? (100.0 * (double)(sharedDone + doneBytes) / (double)grandTotal)
                                         : 0.0;
                        pi.speedBps = elapsed > 0.1 ? (double)(sharedDone + doneBytes) / elapsed : 0.0;
                        if (pi.speedBps > 1.0 && grandTotal > 0)
                        {
                            double remain = (double)grandTotal - (double)(sharedDone + doneBytes);
                            pi.etaSeconds = remain / pi.speedBps;
                        }
                        onProgress(pi);
                        (void)itemBase;
                    };
                }

                MigrateSubtask(rule, names[idx], info, cb, cancel, onProgress);

                // 汇总（锁 rmtx 由调用线程顺序写，worker 内直接自增不安全 → 用 pmtx）
                {
                    std::lock_guard<std::mutex> lk(pmtx);
                    switch (info.state)
                    {
                    case SubtaskState::Migrated:
                        result.migrated++;
                        result.bytesMoved += info.bytes;
                        break;
                    case SubtaskState::Skipped:
                        result.skipped++;
                        break;
                    case SubtaskState::Locked:
                        result.locked++;
                        break;
                    case SubtaskState::Failed:
                        result.failed++;
                        break;
                    default:
                        break;
                    }
                    sharedDone += stats[idx].totalBytes;
                    doneItems++;
                }

                if (info.state == SubtaskState::Locked && onLock)
                {
                    LockNotice n;
                    n.subtask = info.name;
                    n.locks = info.locks;
                    n.reason = info.message;
                    onLock(n);
                }
            }
        };

        int nthreads = cfg_.workerThreads;
        if (nthreads <= 0) // 0 = 自动：按 CPU 核心数（上限 8）
        {
            unsigned hw = std::thread::hardware_concurrency();
            nthreads = (hw == 0) ? 4 : (int)hw;
            if (nthreads > 8)
                nthreads = 8;
        }
        if (nthreads < 1)
            nthreads = 1;
        if (nthreads > 64)
            nthreads = 64;

        std::vector<std::thread> pool;
        for (int i = 0; i < nthreads; ++i)
            pool.emplace_back(worker);
        for (auto &t : pool)
            t.join();

        if (onProgress)
        {
            ProgressInfo pi;
            pi.totalItems = result.total;
            pi.doneItems = result.total;
            pi.totalBytes = grandTotal;
            pi.doneBytes = grandTotal;
            pi.phase = L"完成";
            pi.percent = 100.0;
            onProgress(pi);
        }
        return result;
    }

    MigrateResult MigrateEngine::Migrate(ProgressListener onProgress,
                                         std::atomic<bool> *cancel,
                                         LockListener onLock,
                                         SkipFn skip)
    {
        MigrateResult total;
        int skippedRules = 0;
        for (auto &rule : cfg_.rules)
        {
            if (!rule.enabled)
                continue;
            if (cancel && cancel->load())
                break;

            // 自动/定时迁移：只处理「正确可用」的规则，逐条跳过不可用规则
            std::wstring verr = cfg_.ValidateOneRule(rule);
            if (!verr.empty())
            {
                LOGW(L"[跳过规则] " + rule.watchDir + L" : " + verr);
                skippedRules++;
                continue;
            }

            auto r = MigrateRule(rule, onProgress, cancel, onLock, skip);
            total.total += r.total;
            total.migrated += r.migrated;
            total.skipped += r.skipped;
            total.failed += r.failed;
            total.locked += r.locked;
            total.bytesMoved += r.bytesMoved;
            total.subtasks.insert(total.subtasks.end(), r.subtasks.begin(), r.subtasks.end());
        }
        if (skippedRules > 0)
            LOGW(L"本次迁移跳过 " + std::to_wstring(skippedRules) + L" 条不可用规则");
        return total;
    }

    // ---------------------------------------------------------------------------
    // 恢复
    // ---------------------------------------------------------------------------
    MigrateResult MigrateEngine::RestoreRule(const Rule &rule,
                                             ProgressListener onProgress,
                                             std::atomic<bool> *cancel)
    {
        MigrateResult result;
        auto recs = ReadLinkLog(rule.targetDir);
        result.total = (int)recs.size();
        if (recs.empty())
            return result;

        int done = 0;
        for (auto &r : recs)
        {
            if (cancel && cancel->load())
                break;
            if (onProgress)
            {
                ProgressInfo pi;
                pi.totalItems = result.total;
                pi.doneItems = done;
                pi.currentItem = r.src;
                pi.phase = L"恢复中";
                pi.percent = result.total ? 100.0 * done / result.total : 0.0;
                onProgress(pi);
            }

            if (!Path::DirectoryExists(r.dst))
            {
                LOGW(L"[跳过] 数据源不存在: " + r.dst);
                result.skipped++;
                done++;
                continue;
            }
            if (Path::IsReparsePoint(r.src))
            {
                FileEngine::RemoveLink(r.src, nullptr);
            }
            else if (Path::DirectoryExists(r.src))
            {
                if (!FileEngine::IsEmptyDir(r.src))
                {
                    LOGW(L"[跳过] 原位置仍有实体目录: " + r.src);
                    result.skipped++;
                    done++;
                    continue;
                }
                RemoveDirectoryW(r.src.c_str());
            }

            std::wstring err;
            int failures = FileEngine::MoveTree(r.dst, r.src, nullptr, cancel, &err);
            if (failures == 0)
            {
                result.migrated++;
                DirStat st;
                FileEngine::Scan(r.src, st, nullptr);
                result.bytesMoved += st.totalBytes;
                LOGI(L"[恢复] " + r.src);
            }
            else
            {
                result.failed++;
                LOGE(L"[恢复失败] " + r.src + L" : " + err);
            }
            done++;
        }
        return result;
    }

    // ---------------------------------------------------------------------------
    // 恢复单条规则（详细报告，逐目录，失败不中断）
    // ---------------------------------------------------------------------------
    MigrateEngine::RestoreReport MigrateEngine::RestoreRuleDetailed(
        const Rule &rule, const std::vector<std::wstring> &onlyDirs,
        ProgressListener onProgress, std::atomic<bool> *cancel)
    {
        RestoreReport rep;

        // 构造待还原列表（源目录 + 目标目录）
        struct Item
        {
            std::wstring name, src, dst;
        };
        std::vector<Item> items;

        auto inOnly = [&](const std::wstring &name)
        {
            if (onlyDirs.empty())
                return true;
            for (auto &d : onlyDirs)
                if (Path::Equals(d, name))
                    return true;
            return false;
        };

        // 优先依据日志；再补充 migrated 清单与目标目录里的文件夹
        std::vector<std::wstring> names;
        for (auto &r : ReadLinkLog(rule.targetDir))
        {
            std::wstring nm = Path::FileName(r.src);
            if (inOnly(nm) && !cfg_.IsExcluded(nm, rule))
                names.push_back(nm);
        }
        for (auto &m : rule.migrated)
            if (inOnly(m) && !cfg_.IsExcluded(m, rule))
                names.push_back(m);
        // 目标目录下的文件夹（兜底）
        if (!rule.targetDir.empty() && Path::DirectoryExists(rule.targetDir))
        {
            for (auto &sub : Path::ListSubDirectories(rule.targetDir))
                if (inOnly(sub) && !cfg_.IsExcluded(sub, rule))
                    names.push_back(sub);
        }
        // 去重
        std::vector<std::wstring> uniq;
        for (auto &n : names)
        {
            bool dup = false;
            for (auto &u : uniq)
                if (Path::Equals(u, n))
                {
                    dup = true;
                    break;
                }
            if (!dup)
                uniq.push_back(n);
        }

        for (auto &n : uniq)
        {
            Item it;
            it.name = n;
            it.src = Path::Combine(rule.watchDir, n);
            it.dst = Path::Combine(rule.targetDir, n);
            items.push_back(it);
        }

        rep.total = (int)items.size();
        int done = 0;
        for (auto &it : items)
        {
            if (cancel && cancel->load())
                break;
            if (onProgress)
            {
                ProgressInfo pi;
                pi.totalItems = rep.total;
                pi.doneItems = done;
                pi.currentItem = it.name;
                pi.phase = L"还原中";
                pi.percent = rep.total ? 100.0 * done / rep.total : 0.0;
                onProgress(pi);
            }

            // 源不是链接且目标无数据 → 无需还原
            bool srcIsLink = Path::IsReparsePoint(it.src);
            bool srcIsDir = Path::DirectoryExists(it.src) && !srcIsLink;
            bool dstExists = Path::DirectoryExists(it.dst);

            if (!srcIsLink && !dstExists)
            {
                rep.skipped++;
                done++;
                continue; // 本来就没迁移
            }
            if (srcIsDir && !FileEngine::IsEmptyDir(it.src) && dstExists)
            {
                // 源是实体目录且目标也有数据：无法直接还原，记为失败（避免破坏）
                rep.failedDirs.push_back(it.name);
                rep.failMessages.push_back(L"源目录已是实体且目标存在同名数据");
                LOGE(L"[还原失败] " + it.name + L" : 源实体与目标同时存在");
                done++;
                continue;
            }

            // 删除链接（或空目录）以便搬回
            if (srcIsLink)
                FileEngine::RemoveLink(it.src, nullptr);
            else if (srcIsDir && FileEngine::IsEmptyDir(it.src))
                RemoveDirectoryW(it.src.c_str());

            if (!dstExists)
            {
                // 目标没数据（可能已在源）：视为已完成
                rep.skipped++;
                done++;
                continue;
            }

            std::wstring err;
            int failures = FileEngine::MoveTree(it.dst, it.src, nullptr, cancel, &err);
            if (failures == 0)
            {
                rep.succeeded++;
                // 还原成功后从日志/清单中移除
                LOGI(L"[还原] " + it.name);
            }
            else
            {
                rep.failedDirs.push_back(it.name);
                rep.failMessages.push_back(err.empty() ? L"移动失败" : err);
                LOGE(L"[还原失败] " + it.name + L" : " + err);
                // 尽量恢复链接，避免源目录残缺
                bool isJunction = (cfg_.linkType == LinkType::Junction);
                if (!Path::DirectoryExists(it.src))
                    FileEngine::CreateLink(it.src, it.dst, isJunction, nullptr);
            }
            done++;
        }
        return rep;
    }

    MigrateResult MigrateEngine::Restore(ProgressListener onProgress,
                                         std::atomic<bool> *cancel)
    {
        MigrateResult total;
        for (auto &rule : cfg_.rules)
        {
            if (!rule.enabled)
                continue;
            if (cancel && cancel->load())
                break;
            auto r = RestoreRule(rule, onProgress, cancel);
            total.total += r.total;
            total.migrated += r.migrated;
            total.skipped += r.skipped;
            total.failed += r.failed;
            total.bytesMoved += r.bytesMoved;
        }
        return total;
    }

    // ---------------------------------------------------------------------------
    // 修改目标目录：旧目标已迁移数据整体移动到新目标 + 重建链接
    // ---------------------------------------------------------------------------
    std::wstring MigrateEngine::ChangeTargetDir(const Rule &rule,
                                                const std::wstring &newTargetDir,
                                                ProgressListener onProgress,
                                                std::atomic<bool> *cancel)
    {
        std::wstring oldTarget = Path::Normalize(rule.targetDir);
        std::wstring newTarget = Path::Normalize(newTargetDir);

        if (Path::Equals(oldTarget, newTarget))
            return L"新目标目录与旧目标目录相同";
        if (!Path::DirectoryExists(newTarget))
            return L"新目标目录不存在: " + newTarget;

        // 读取旧目标日志中的链接记录
        auto recs = ReadLinkLog(oldTarget);
        if (recs.empty())
            return L""; // 没有已迁移数据，直接由调用方更新配置即可

        int total = (int)recs.size();
        int done = 0;

        for (auto &r : recs)
        {
            if (cancel && cancel->load())
                return L"已取消";

            if (onProgress)
            {
                ProgressInfo pi;
                pi.totalItems = total;
                pi.doneItems = done;
                pi.currentItem = r.src;
                pi.phase = L"更换目标目录";
                pi.percent = total ? 100.0 * done / total : 0.0;
                onProgress(pi);
            }

            std::wstring name = Path::FileName(r.src);
            std::wstring newDst = Path::Combine(newTarget, name);

            if (!Path::DirectoryExists(r.dst))
            {
                // 旧数据不在（可能已手动处理），跳过
                LOGW(L"[更换目标] 跳过，旧数据不存在: " + r.dst);
                done++;
                continue;
            }
            if (Path::DirectoryExists(newDst))
            {
                return L"新目标中已存在同名目录，无法更换: " + newDst;
            }

            // 1) 把旧目标数据移动到新目标
            std::wstring err;
            int failures = FileEngine::MoveTree(r.dst, newDst, nullptr, cancel, &err);
            if (failures != 0)
            {
                // 回滚：尽力移回
                if (Path::DirectoryExists(newDst) && !Path::DirectoryExists(r.dst))
                    FileEngine::MoveTree(newDst, r.dst, nullptr, nullptr, &err);
                return L"移动数据到新目标失败: " + err;
            }

            // 2) 删除源目录的旧链接，重建指向新目标
            if (Path::IsReparsePoint(r.src))
                FileEngine::RemoveLink(r.src, nullptr);
            std::wstring lerr;
            bool isJunction = (cfg_.linkType == LinkType::Junction);
            if (!FileEngine::CreateLink(r.src, newDst, isJunction, &lerr))
            {
                // 回滚：把数据移回旧目标并恢复旧链接
                FileEngine::MoveTree(newDst, r.dst, nullptr, nullptr, &err);
                std::wstring rlerr;
                FileEngine::CreateLink(r.src, r.dst, isJunction, &rlerr);
                return L"重建链接失败: " + lerr;
            }
            LOGI(L"[更换目标] " + name + L" -> " + newDst);
            done++;
        }

        // 3) 重写新目标日志（内容与旧日志一致，只是目标目录变了）
        WriteLogHeader(newTarget);
        for (auto &r : recs)
        {
            std::wstring name = Path::FileName(r.src);
            std::wstring newDst = Path::Combine(newTarget, name);
            AppendLinkLog(newTarget, r.src, newDst);
        }

        // 4) 清理旧目标日志
        std::wstring oldLog = LogPathFor(oldTarget);
        if (Path::FileExists(oldLog))
            DeleteFileW(oldLog.c_str());

        return L"";
    }

    // ---------------------------------------------------------------------------
    // 取消排除：若该目录已被迁移(是链接)，还原它（搬回源、删链接）
    // ---------------------------------------------------------------------------
    std::wstring MigrateEngine::RestoreExcluded(const Rule &rule,
                                                const std::wstring &name,
                                                std::atomic<bool> *cancel)
    {
        std::wstring src = Path::Combine(rule.watchDir, name);
        std::wstring dst = Path::Combine(rule.targetDir, name);

        // 情况 A：源目录本身是链接（标准迁移状态）→ 还原
        if (Path::IsReparsePoint(src))
        {
            if (!Path::DirectoryExists(dst))
                return L"目标数据不存在，无法还原: " + dst;
            std::wstring err;
            FileEngine::RemoveLink(src, nullptr);
            int failures = FileEngine::MoveTree(dst, src, nullptr, cancel, &err);
            if (failures != 0)
            {
                // 尝试恢复链接
                bool isJunction = (cfg_.linkType == LinkType::Junction);
                FileEngine::CreateLink(src, dst, isJunction, nullptr);
                return L"还原失败: " + err;
            }
            LOGI(L"[还原白名单目录] " + name);
            return L"";
        }

        // 情况 B：源目录是实体，但目标里也有同名数据（历史遗留）→ 不处理，提示
        if (Path::DirectoryExists(src) && Path::DirectoryExists(dst) &&
            !(cfg_.skipExisting))
        {
            return L"源目录已是实体且目标存在同名数据，跳过还原";
        }
        return L""; // 无需还原
    }

    // ---------------------------------------------------------------------------
    // 扫描接管：补全 migrated 清单；找出外部链接；列出普通目录
    // ---------------------------------------------------------------------------
    AdoptResult MigrateEngine::AdoptExisting(Rule &rule) const
    {
        AdoptResult res;
        if (rule.watchDir.empty() || !Path::DirectoryExists(rule.watchDir))
            return res;

        std::wstring watchNorm = Path::Normalize(rule.watchDir);
        std::wstring targetNorm = Path::Normalize(rule.targetDir);

        auto subs = Path::ListSubDirectories(watchNorm);
        for (auto &name : subs)
        {
            std::wstring full = Path::Combine(watchNorm, name);
            bool isExcluded = cfg_.IsExcluded(name, rule);

            if (Path::IsReparsePoint(full))
            {
                // 是链接：判断它指向哪里
                std::wstring tgt = Path::ReadLinkTarget(full);
                std::wstring tgtNorm = Path::Normalize(tgt);

                bool pointsToOurTarget = !targetNorm.empty() &&
                                         (Path::Equals(tgtNorm, Path::Combine(targetNorm, name)) ||
                                          Path::IsSubPath(targetNorm, tgtNorm) ||
                                          Path::Equals(tgtNorm, targetNorm));

                if (pointsToOurTarget)
                {
                    // 属于本规则的既有迁移：纳入（若尚未记录）
                    if (!isExcluded)
                    {
                        bool known = false;
                        for (auto &m : rule.migrated)
                            if (Path::Equals(m, name))
                            {
                                known = true;
                                break;
                            }
                        if (!known)
                        {
                            rule.migrated.push_back(name);
                            res.adopted.push_back(name);
                        }
                    }
                }
                else
                {
                    // 指向非本目录的链接：需提醒
                    ForeignLink fl;
                    fl.linkPath = full;
                    fl.target = tgt;
                    res.foreignLinks.push_back(fl);
                }
            }
            else if (Path::DirectoryExists(full))
            {
                res.plainDirs.push_back(name);
                // 目标里存在同名数据，但源仍是实体（历史遗留），也提示
                if (!targetNorm.empty())
                {
                    std::wstring tgtFull = Path::Combine(targetNorm, name);
                    if (Path::DirectoryExists(tgtFull))
                    {
                        ForeignLink fl;
                        fl.linkPath = full;
                        fl.target = tgtFull;
                        res.foreignLinks.push_back(fl);
                    }
                }
            }
        }

        // 同时检查目标目录里是否有多余的文件夹（源中没有链接对应）
        if (!targetNorm.empty() && Path::DirectoryExists(targetNorm))
        {
            auto tsubs = Path::ListSubDirectories(targetNorm);
            for (auto &tn : tsubs)
            {
                std::wstring srcFull = Path::Combine(watchNorm, tn);
                bool srcIsLink = Path::IsReparsePoint(srcFull);
                if (!srcIsLink)
                {
                    // 目标有数据但源不是链接 —— 记录为已迁移候选（源可能是实体遗留）
                    bool known = false;
                    for (auto &m : rule.migrated)
                        if (Path::Equals(m, tn))
                        {
                            known = true;
                            break;
                        }
                    // 不自动纳入，交由上层提示（避免误判）
                    (void)known;
                }
            }
        }
        return res;
    }

    // ---------------------------------------------------------------------------
    // 从目标目录的配置/清单，在新机器重建链接
    // ---------------------------------------------------------------------------
    std::wstring MigrateEngine::RebuildLinksFromTarget(const Rule &rule,
                                                       const std::wstring &newWatchDir,
                                                       ProgressListener onProgress,
                                                       std::atomic<bool> *cancel) const
    {
        std::wstring watch = Path::Normalize(newWatchDir);
        std::wstring target = Path::Normalize(rule.targetDir);
        if (watch.empty())
            return L"未指定新的监视目录";
        if (target.empty() || !Path::DirectoryExists(target))
            return L"目标目录不存在: " + target;

        if (!FileEngine::EnsureDirectory(watch))
            return L"无法创建监视目录: " + watch;

        // 优先使用规则清单（migrated）；为空则扫描目标目录下的文件夹
        std::vector<std::wstring> names = rule.migrated;
        if (names.empty())
        {
            // 从目标目录日志读取
            auto recs = ReadLinkLog(target);
            for (auto &r : recs)
                names.push_back(Path::FileName(r.src));
            // 仍为空则扫描目标目录文件夹
            if (names.empty())
            {
                auto subs = Path::ListSubDirectories(target);
                for (auto &s : subs)
                    names.push_back(s);
            }
        }

        int total = (int)names.size();
        int done = 0;
        bool isJunction = (cfg_.linkType == LinkType::Junction);

        for (auto &name : names)
        {
            if (cancel && cancel->load())
                return L"已取消";
            if (onProgress)
            {
                ProgressInfo pi;
                pi.totalItems = total;
                pi.doneItems = done;
                pi.currentItem = name;
                pi.phase = L"重建链接";
                pi.percent = total ? 100.0 * done / total : 0.0;
                onProgress(pi);
            }

            std::wstring dstFull = Path::Combine(target, name);
            if (!Path::DirectoryExists(dstFull))
            {
                done++;
                continue;
            }
            std::wstring srcFull = Path::Combine(watch, name);

            // 源已存在且是链接：确认指向；不是则跳过（避免破坏）
            if (Path::IsReparsePoint(srcFull))
            {
                std::wstring cur = Path::Normalize(Path::ReadLinkTarget(srcFull));
                if (Path::Equals(cur, Path::Normalize(dstFull)))
                {
                    done++;
                    continue; // 已正确
                }
                FileEngine::RemoveLink(srcFull, nullptr);
            }
            else if (Path::DirectoryExists(srcFull))
            {
                // 源是实体目录：不擅自覆盖，跳过
                LOGW(L"[重建链接] 跳过（源已存在实体目录）: " + srcFull);
                done++;
                continue;
            }

            std::wstring lerr;
            if (!FileEngine::CreateLink(srcFull, dstFull, isJunction, &lerr))
                return L"创建链接失败 " + name + L": " + lerr;
            LOGI(L"[重建链接] " + name + L" -> " + dstFull);
            done++;
        }

        // 在该目录下写一份规则配置（含 migrated 清单）
        Config one = cfg_;
        one.rules.clear();
        Rule r2 = rule;
        r2.watchDir = watch;
        r2.migrated = names;
        one.rules.push_back(r2);
        ConfigStore::SyncToRuleDirs(one);
        return L"";
    }

} // namespace dirmv
