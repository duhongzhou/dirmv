#pragma once
// ---------------------------------------------------------------------------
// Config.h - 应用配置（INI 格式，UTF-8）
//   多条「监视目录 -> 目标目录」规则；每条规则独立白名单（无全局白名单）
//   配置文件在 监视目录 与 目标目录 各保存一份（内容相同）
// ---------------------------------------------------------------------------
#include <string>
#include <vector>
#include <cstdint>

namespace dirmv
{

    enum class LinkType
    {
        SymbolicLink = 0,
        Junction = 1
    };

    // 一条迁移规则：一个监视目录 -> 一个目标目录，带独立白名单
    struct Rule
    {
        std::wstring watchDir;  // 被监视的源根目录
        std::wstring targetDir; // 迁移到的目标根目录（数据实际存放处）
        // 该规则自己的白名单：这些一级子目录不迁移（不区分大小写）
        std::vector<std::wstring> exclude;
        bool enabled = false; // 默认不启用

        // 子目录清单：记录监视目录下所有一级子目录及其状态
        // 用于「接管已有迁移」与「新机器重建链接」
        std::vector<std::wstring> migrated; // 已迁移(源为链接、指向本规则目标)

        // 待删除：删除规则时部分目录还原失败，规则被标记为待删除，
        // 需重试还原；全部还原成功后才真正删除。
        bool pendingDelete = false;
        // 待删除规则中还原失败的目录名（重试时只处理这些）
        std::vector<std::wstring> pendingDeleteFailed;

        Rule() = default;
        Rule(std::wstring w, std::wstring t) : watchDir(std::move(w)), targetDir(std::move(t)) {}
    };

    // 已有的链接但其目标不在本规则目标目录内 —— 需要提醒
    struct ForeignLink
    {
        std::wstring linkPath; // 链接所在路径（监视目录下）
        std::wstring target;   // 它当前指向的目标
    };

    // 扫描接管结果
    struct AdoptResult
    {
        std::vector<std::wstring> adopted;     // 新增纳入的已迁移目录名
        std::vector<ForeignLink> foreignLinks; // 指向非本目录的链接（提醒）
        std::vector<std::wstring> plainDirs;   // 普通实体目录（未迁移）
    };

    // 定时调度：单一规则，固定周期 + 固定时刻
    enum class SchedulePeriod
    {
        Disabled = 0,   // 不定时（仅实时监视/手动）
        Daily = 1,      // 每天
        Weekly = 2,     // 每周（按 weekdayMask 选星期几）
        Monthly = 3,    // 每月（按 dayOfMonth 选某天）
        EveryNDays = 4, // 每 N 天
    };

    struct Schedule
    {
        SchedulePeriod period = SchedulePeriod::Disabled;
        int hour = 2;        // 0-23 时间点
        int minute = 0;      // 0-59 时间点
        int weekdayMask = 0; // 周模式：bit0=周日 .. bit6=周六
        int dayOfMonth = 1;  // 月模式：1-28
        int everyNDays = 3;  // 每 N 天模式
    };

    struct Config
    {
        // 多条迁移规则
        std::vector<Rule> rules;

        // 迁移参数
        LinkType linkType = LinkType::SymbolicLink; // 默认符号链接
        bool moveMode = true;                       // true=移动(迁移后源目录清空)；false=复制保留
        bool skipExisting = true;                   // 目标已存在同名目录时跳过
        int workerThreads = 4;                      // 前台线程数上限（1..64）
        bool enableWatcher = true;                  // ReadDirectoryChangesW 实时监视

        // 定时调度
        Schedule schedule;

        // 占用重试
        bool lockDetectEnabled = true;
        int retryDelayMinutes = 5;
        bool notifyOnLock = true;

        // 服务
        std::wstring serviceName = L"dirMoverSvc";
        int serviceThreads = 4;

        // 日志
        bool logToFile = true;
        int logLevel = 1; // 0 Trace 1 Info 2 Warn 3 Error

        // 生成默认配置：一条「当前用户目录」规则（不启用、无目标、带默认白名单）
        void ApplyDefaults();

        // 默认白名单（用于新规则）
        static std::vector<std::wstring> DefaultExclude();

        // 校验整个配置：返回空串表示通过，否则为错误信息
        std::wstring Validate() const;
        // 校验全部规则（重叠 / 目标重复 / 存在性等）
        std::wstring ValidateRules() const;

        // 校验单条规则自身是否可用（存在性、路径合法性、目标可写等）。
        // 返回空串=该规则可用于迁移。
        std::wstring ValidateOneRule(const Rule &rule) const;

        // 判断某个一级目录名是否被某条规则排除
        bool IsExcluded(const std::wstring &name, const Rule &rule) const;
    };

    // 配置读写（INI）
    // 保存位置共三个：
    //   1) 应用程序配置目录：%ProgramData%\dirMover\dirmv.ini（失败回退到 exe 目录）
    //   2) 每条规则的监视目录：<watchDir>\.dirmv\dirmv.ini
    //   3) 每条规则的目标目录：<targetDir>\.dirmv\dirmv.ini
    class ConfigStore
    {
    public:
        // 应用程序配置目录（%ProgramData%\dirMover），不存在则尝试创建
        static std::wstring AppConfigDir();
        // 主配置路径（应用程序配置目录下的 dirmv.ini）
        static std::wstring DefaultPath();

        // 覆盖主配置路径（供 --config 使用，测试时可指向沙箱配置）
        static void SetOverridePath(const std::wstring &path);
        static bool HasOverridePath();
        static std::wstring OverridePath();

        static bool Load(const std::wstring &path, Config &out);
        // 保存：写应用程序配置目录（含全部规则，签名）+
        //       每条规则的监视目录（只含该规则）+ 目标目录（只含该规则）
        static bool Save(const std::wstring &path, const Config &cfg);

        // 仅写应用程序配置目录（全部规则，带签名）
        static bool SaveMain(const std::wstring &path, const Config &cfg);
        // 把配置同步写到各规则的监视目录/目标目录（各自只含本规则，带签名）
        static void SyncToRuleDirs(const Config &cfg);

        // 某个目录下配置文件的路径（<dir>\.dirmv\dirmv.ini）
        static std::wstring ConfigPathInDir(const std::wstring &dir);

        // 只包含单条规则的配置文本（带签名）——用于写入监视/目标目录
        static std::wstring SerializeSingleRule(const Config &cfg, const Rule &rule);

        // 从某目录读取配置（校验签名）。返回：0 成功
        // tampered=true 表示签名不匹配（已给出原文）
        static int LoadFromDir(const std::wstring &dir, Config &out, bool *tampered);

        // 导入来源类型（对应三种保存位置）
        enum class ImportKind
        {
            None = 0,     // 无配置
            WatchConfig,  // 该目录是规则的监视目录（配置文件里 WatchDir == 该目录）
            TargetConfig, // 该目录是规则的目标目录（配置文件里 TargetDir == 该目录）
            Relocated,    // 该目录含配置，但配置里记录的路径都不是该目录
                          // （典型：把目标目录整体复制/搬到了新位置 → 需要把
                          //   配置里的目标目录替换为当前真实目录，并重指定监视目录）
        };

        // 判断某目录下配置文件属于哪种导入来源
        static ImportKind ClassifyDir(const std::wstring &dir);

        // 从某目录导入规则（读取 + 校验签名）：
        //   outRules       读到的规则（可能多条）
        //   kind           来源类型
        //   tampered       签名是否不匹配
        //   watchMatched   规则中的监视目录是否与该目录一致（目标目录配置通常不一致）
        // 返回空串=成功，否则为错误信息。
        static std::wstring ImportFromDir(const std::wstring &dir,
                                          std::vector<Rule> &outRules,
                                          ImportKind &kind,
                                          bool &tampered,
                                          bool &watchMatched);
    };

    // 字符串序列化（供双份保存共享）
    std::wstring SerializeConfig(const Config &cfg);
    bool ParseConfigText(const std::wstring &text, Config &out);

} // namespace dirmv
