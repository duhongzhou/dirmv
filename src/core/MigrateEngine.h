#pragma once
// ---------------------------------------------------------------------------
// MigrateEngine.h - 迁移 / 恢复 编排引擎（多规则 + 子任务独立 + 原子性）
//   规则 = 监视目录 -> 目标目录；
//   每个监视目录下每个非白名单一级目录 = 独立「子任务」：
//     · 子任务之间互不影响，一个失败不回滚其他子任务
//     · 单个子任务原子：要么完整迁移成功，要么完整回滚到原样（绝不半迁移）
//   占用检测：迁移前检测被锁定文件；被占用则该子任务失败(不半迁移)，
//             交由上层弹窗/通知并创建一次性重试计划
// ---------------------------------------------------------------------------
#include "core/Config.h"
#include "core/FileEngine.h"

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>

namespace dirmv
{

    // 单个子任务（一个顶层目录）的执行结果
    enum class SubtaskState
    {
        Pending = 0,
        Migrated, // 迁移成功
        Skipped,  // 白名单 / 已存在 / 已是链接
        Locked,   // 检测到占用
        Failed,   // 失败（已回滚）
    };

    struct SubtaskInfo
    {
        std::wstring name; // 顶层目录名
        std::wstring src;
        std::wstring dst;
        SubtaskState state = SubtaskState::Pending;
        std::wstring message; // 失败/占用说明
        uint64_t bytes = 0;
        // 占用进程（Locked 时）
        std::vector<LockInfo> locks;
    };

    // 迁移结果（一次扫描的全部子任务汇总）
    struct MigrateResult
    {
        int total = 0;
        int migrated = 0;
        int skipped = 0;
        int failed = 0;
        int locked = 0;
        uint64_t bytesMoved = 0;
        std::vector<SubtaskInfo> subtasks;
    };

    // 进度快照
    struct ProgressInfo
    {
        int totalItems = 0;
        int doneItems = 0;
        uint64_t totalBytes = 0;
        uint64_t doneBytes = 0;
        std::wstring currentItem;
        std::wstring phase; // 扫描中 / 迁移中 / 创建链接
        double percent = 0.0;
        double etaSeconds = -1;
        double speedBps = 0.0;
    };

    using ProgressListener = std::function<void(const ProgressInfo &)>;

    // 占用/失败通知回调：子任务名 + 被占用文件 + 占用进程
    struct LockNotice
    {
        std::wstring subtask;
        std::vector<LockInfo> locks;
        std::wstring reason; // 供显示
    };
    using LockListener = std::function<void(const LockNotice &)>;

    class MigrateEngine
    {
    public:
        explicit MigrateEngine(const Config &cfg);

        // 扫描某条规则的源目录，得到待迁移目录名列表
        std::vector<std::wstring> ScanPending(const Rule &rule) const;

        // 执行全部启用规则的迁移（阻塞）
        MigrateResult Migrate(ProgressListener onProgress,
                              std::atomic<bool> *cancel = nullptr,
                              LockListener onLock = nullptr);

        // 仅执行指定规则（服务/定时按规则触发）
        MigrateResult MigrateRule(const Rule &rule, ProgressListener onProgress,
                                  std::atomic<bool> *cancel = nullptr,
                                  LockListener onLock = nullptr);

        // 从日志恢复：把所有 LINK 记录的数据移回原位置
        MigrateResult Restore(ProgressListener onProgress,
                              std::atomic<bool> *cancel = nullptr);

        // 从单条规则恢复（把该规则已迁移的目录搬回原位置）
        MigrateResult RestoreRule(const Rule &rule, ProgressListener onProgress,
                                  std::atomic<bool> *cancel = nullptr);

        // 恢复单条规则的详细报告（用于删除规则前的还原）
        struct RestoreReport
        {
            int total = 0;
            int succeeded = 0;
            int skipped = 0;
            std::vector<std::wstring> failedDirs;   // 还原失败的目录名
            std::vector<std::wstring> failMessages; // 与 failedDirs 对应
        };
        // 只还原指定的目录集合（若 dirs 为空则还原该规则全部文件）
        RestoreReport RestoreRuleDetailed(const Rule &rule,
                                          const std::vector<std::wstring> &onlyDirs,
                                          ProgressListener onProgress = nullptr,
                                          std::atomic<bool> *cancel = nullptr);

        // 修改目标目录：把该规则旧目标下的已迁移数据整体移动到新目标，并重建链接
        // 返回空串=成功。
        std::wstring ChangeTargetDir(const Rule &rule, const std::wstring &newTargetDir,
                                     ProgressListener onProgress = nullptr,
                                     std::atomic<bool> *cancel = nullptr);

        // 取消排除（把某目录加入白名单前调用）：若该目录已被迁移(是链接)则还原
        // （搬回源目录、删除链接），使其回到监视状态。返回空串=成功。
        std::wstring RestoreExcluded(const Rule &rule, const std::wstring &name,
                                     std::atomic<bool> *cancel = nullptr);

        // 日志路径（每个目标目录一个日志文件）
        std::wstring LogPathFor(const std::wstring &targetDir) const;

        // 扫描接管：检测监视目录下已有的链接与目标目录下的文件夹，
        // 补全规则的 migrated 清单；返回需提醒的外部链接等。
        // 不修改磁盘，只分析并更新 rule.migrated（引用传入）。
        AdoptResult AdoptExisting(Rule &rule) const;

        // 从目标目录中的配置/清单，在新机器上重建链接。
        // newWatchDir：新的监视目录（完全手动指定）。
        // 返回空串=成功；否则为错误信息。
        std::wstring RebuildLinksFromTarget(const Rule &rule,
                                            const std::wstring &newWatchDir,
                                            ProgressListener onProgress = nullptr,
                                            std::atomic<bool> *cancel = nullptr) const;

        static constexpr const wchar_t *LOG_HEADER = L"LINK_MIGRATE_LOG";

    private:
        Config cfg_;

        void WriteLogHeader(const std::wstring &targetDir);
        void AppendLinkLog(const std::wstring &targetDir,
                           const std::wstring &src, const std::wstring &dst);

        struct LinkRec
        {
            std::wstring src, dst, targetDir;
        };
        std::vector<LinkRec> ReadLinkLog(const std::wstring &targetDir) const;

        // 迁移单个子任务（原子：失败完整回滚），更新 info
        void MigrateSubtask(const Rule &rule, const std::wstring &name,
                            SubtaskInfo &info, ProgressFn cb,
                            std::atomic<bool> *cancel, ProgressListener onProgress);
    };

} // namespace dirmv
