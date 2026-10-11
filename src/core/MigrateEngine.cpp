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

    std::vector<std::wstring> MigrateEngine::ScanPending(const Rule &rule) const
    {
        std::vector<std::wstring> pending;
        auto subdirs = Path::ListSubDirectories(rule.watchDir);
        for (auto &name : subdirs)
        {
            if (cfg_.IsExcluded(name, rule))
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
    // 单个子任务迁移（原子）
    // ---------------------------------------------------------------------------
    void MigrateEngine::MigrateSubtask(const Rule &rule, const std::wstring &name,
                                       SubtaskInfo &info, ProgressFn cb,
                                       std::atomic<bool> *cancel,
                                       ProgressListener onProgress)
    {
        (void)onProgress;
        std::wstring src = Path::Combine(rule.watchDir, name);
        std::wstring dst = Path::Combine(rule.targetDir, name);
        info.name = name;
        info.src = src;
        info.dst = dst;

        DirStat st;
        FileEngine::Scan(src, st, cancel);
        info.bytes = st.totalBytes;

        // --- 占用检测：若任何文件被占用，直接判定该子任务为 Locked（不半迁移）---
        if (cfg_.lockDetectEnabled)
        {
            auto locks = FileEngine::DetectLocks(src);
            if (!locks.empty())
            {
                info.state = SubtaskState::Locked;
                info.locks = std::move(locks);
                info.message = L"检测到 " + std::to_wstring(info.locks.size()) +
                               L" 个被占用的文件";
                LOGW(L"[占用] " + name + L" : " + info.message);
                return;
            }
        }

        // 目标已存在
        if (Path::DirectoryExists(dst) && cfg_.skipExisting)
        {
            info.state = SubtaskState::Skipped;
            info.message = L"目标已存在，跳过";
            return;
        }

        // --- 复制/移动 ---
        std::wstring err;
        int failures = 0;
        if (cfg_.moveMode)
            failures = FileEngine::MoveTree(src, dst, cb, cancel, &err);
        else
            failures = FileEngine::CopyTree(src, dst, cb, cancel, &err);

        if (failures != 0 || (cancel && cancel->load()))
        {
            // 原子回滚：尽力把已复制的数据清掉/整回到原样
            LOGW(L"[失败] " + name + L" : " + (err.empty() ? L"复制出错" : err) + L"，回滚");
            if (cfg_.moveMode)
            {
                // 移动模式：MoveTree 失败可能已部分移动，尝试把存在的目标数据移回
                if (Path::DirectoryExists(dst))
                {
                    // 若源目录仍有内容则需合并；简单起见把目标剩余内容搬回源
                    std::wstring rerr;
                    if (!Path::DirectoryExists(src))
                        FileEngine::MoveTree(dst, src, nullptr, nullptr, &rerr);
                    else
                        FileEngine::RemoveTree(dst, &rerr);
                }
            }
            else
            {
                // 复制模式：源未动，清掉半成品目标
                std::wstring rerr;
                if (Path::DirectoryExists(dst))
                    FileEngine::RemoveTree(dst, &rerr);
            }
            info.state = SubtaskState::Failed;
            info.message = err.empty() ? L"复制出错" : err;
            return;
        }

        // 复制模式下源目录仍在，需删除后再建链接
        if (!cfg_.moveMode)
        {
            std::wstring derr;
            if (!FileEngine::RemoveTree(src, &derr))
            {
                // 删除源失败 → 该子任务失败，回滚目标
                LOGW(L"复制模式删除源目录失败: " + src + L"，回滚");
                FileEngine::RemoveTree(dst, nullptr);
                info.state = SubtaskState::Failed;
                info.message = L"删除源目录失败: " + derr;
                return;
            }
        }

        // --- 创建链接 ---
        std::wstring lerr;
        bool isJunction = (cfg_.linkType == LinkType::Junction);
        if (FileEngine::CreateLink(src, dst, isJunction, &lerr))
        {
            info.state = SubtaskState::Migrated;
            AppendLinkLog(rule.targetDir, src, dst);
            LOGI(L"[完成] " + name);
        }
        else
        {
            // 回滚：把数据整体移回源，保证源目录结构完整
            LOGW(L"[链接失败] " + name + L" : " + lerr + L"，正在回滚");
            std::wstring rerr;
            if (!Path::DirectoryExists(src))
                FileEngine::MoveTree(dst, src, nullptr, nullptr, &rerr);
            else if (Path::DirectoryExists(dst))
                FileEngine::RemoveTree(dst, &rerr);
            info.state = SubtaskState::Failed;
            info.message = lerr;
        }
    }

    // ---------------------------------------------------------------------------
    // 单条规则迁移
    // ---------------------------------------------------------------------------
    MigrateResult MigrateEngine::MigrateRule(const Rule &rule,
                                             ProgressListener onProgress,
                                             std::atomic<bool> *cancel,
                                             LockListener onLock)
    {
        MigrateResult result;

        auto names = ScanPending(rule);
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
                                         LockListener onLock)
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

            auto r = MigrateRule(rule, onProgress, cancel, onLock);
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
