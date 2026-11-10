#include "core/Config.h"
#include "core/PathUtil.h"
#include "core/FileEngine.h"
#include "core/Integrity.h"
#include "core/Log.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")

namespace dirmv
{

    std::vector<std::wstring> Config::DefaultExclude()
    {
        return {
            L"AppData", L"Application Data", L"Contacts", L"Cookies", L"Desktop",
            L"Documents", L"Downloads", L"Favorites", L"Links", L"Local Settings",
            L"Music", L"My Documents", L"NetHood", L"OneDrive", L"Pictures",
            L"PrintHood", L"Recent", L"Saved Games", L"Searches", L"SendTo",
            L"Templates", L"Videos"};
    }

    void Config::ApplyDefaults()
    {
        // 默认一条规则：当前用户目录，未启用、无目标目录、带默认白名单
        rules.clear();
        Rule r;
        wchar_t prof[MAX_PATH] = {0};
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
        r.watchDir = (n > 0 && n < MAX_PATH) ? std::wstring(prof) : L"C:\\Users";
        r.targetDir.clear();
        r.exclude = DefaultExclude();
        r.enabled = false;
        rules.push_back(r);
    }

    bool Config::IsExcluded(const std::wstring &name, const Rule &rule) const
    {
        // 应用自身的元数据目录永远不作为迁移对象
        if (_wcsicmp(name.c_str(), L".dirmv") == 0)
            return true;

        auto ieq = [](const std::wstring &a, const std::wstring &b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::towlower(a[i]) != std::towlower(b[i]))
                    return false;
            return true;
        };
        for (auto &e : rule.exclude)
            if (ieq(e, name))
                return true;
        return false;
    }

    // 单条规则可用性校验（不涉及与其他规则的重叠/重复）
    std::wstring Config::ValidateOneRule(const Rule &r) const
    {
        if (!r.enabled)
            return L"该规则未启用";
        if (r.pendingDelete)
            return L"该规则待删除（存在未还原的目录）";
        if (r.watchDir.empty())
            return L"未设置监视目录";
        if (r.targetDir.empty())
            return L"未设置目标目录";
        if (!Path::IsAbsolute(r.watchDir))
            return L"监视目录必须是绝对路径";
        if (!Path::IsAbsolute(r.targetDir))
            return L"目标目录必须是绝对路径";
        if (Path::IsDriveRoot(r.watchDir))
            return L"监视目录不能是磁盘根目录";
        if (Path::IsDriveRoot(r.targetDir))
            return L"目标目录不能是磁盘根目录";

        std::wstring w = Path::Normalize(r.watchDir);
        std::wstring t = Path::Normalize(r.targetDir);
        if (Path::Equals(w, t))
            return L"监视目录与目标目录不能相同";
        if (Path::IsSubPath(w, t))
            return L"目标目录位于监视目录内部";
        if (Path::IsSubPath(t, w))
            return L"监视目录位于目标目录内部";
        if (!Path::DirectoryExists(w))
            return L"监视目录不存在: " + w;
        if (!FileEngine::EnsureDirectory(t))
            return L"目标目录不可用（无法创建/访问）: " + t;
        return L"";
    }

    std::wstring Config::ValidateRules() const
    {
        if (rules.empty())
            return L"至少需要一条迁移规则（监视目录 -> 目标目录）";

        for (size_t i = 0; i < rules.size(); ++i)
        {
            const Rule &r = rules[i];
            if (!r.enabled)
                continue;
            std::wstring tag = L"第 " + std::to_wstring(i + 1) + L" 条规则";
            if (r.watchDir.empty())
                return tag + L"未设置监视目录";
            if (r.targetDir.empty())
                return tag + L"未设置目标目录";
            if (!Path::IsAbsolute(r.watchDir))
                return tag + L"的监视目录必须是绝对路径";
            if (!Path::IsAbsolute(r.targetDir))
                return tag + L"的目标目录必须是绝对路径";
            if (Path::IsDriveRoot(r.watchDir))
                return tag + L"的监视目录不能是磁盘根目录";
            if (Path::IsDriveRoot(r.targetDir))
                return tag + L"的目标目录不能是磁盘根目录";

            std::wstring w = Path::Normalize(r.watchDir);
            std::wstring t = Path::Normalize(r.targetDir);
            if (Path::Equals(w, t))
                return tag + L"的监视目录与目标目录不能相同";
            if (Path::IsSubPath(w, t))
                return tag + L"的目标目录位于监视目录内部";
            if (Path::IsSubPath(t, w))
                return tag + L"的监视目录位于目标目录内部";
        }

        // 监视目录不能相互重叠
        for (size_t i = 0; i < rules.size(); ++i)
        {
            if (!rules[i].enabled)
                continue;
            std::wstring wi = Path::Normalize(rules[i].watchDir);
            for (size_t j = i + 1; j < rules.size(); ++j)
            {
                if (!rules[j].enabled)
                    continue;
                std::wstring wj = Path::Normalize(rules[j].watchDir);
                if (Path::Equals(wi, wj))
                    return L"监视目录重复: " + wi;
                if (Path::IsSubPath(wi, wj) || Path::IsSubPath(wj, wi))
                    return L"监视目录不能相互重叠: " + wi + L" 与 " + wj;
            }
        }

        // 目标目录不能重复 / 相互包含
        for (size_t i = 0; i < rules.size(); ++i)
        {
            if (!rules[i].enabled)
                continue;
            std::wstring ti = Path::Normalize(rules[i].targetDir);
            for (size_t j = i + 1; j < rules.size(); ++j)
            {
                if (!rules[j].enabled)
                    continue;
                std::wstring tj = Path::Normalize(rules[j].targetDir);
                if (Path::Equals(ti, tj))
                    return L"目标目录被多条规则重复使用: " + ti;
                if (Path::IsSubPath(ti, tj) || Path::IsSubPath(tj, ti))
                    return L"目标目录不能相互包含: " + ti + L" 与 " + tj;
            }
        }

        // 目标目录不能落在任何监视目录内
        for (size_t i = 0; i < rules.size(); ++i)
        {
            if (!rules[i].enabled)
                continue;
            std::wstring ti = Path::Normalize(rules[i].targetDir);
            for (size_t j = 0; j < rules.size(); ++j)
            {
                if (!rules[j].enabled)
                    continue;
                std::wstring wj = Path::Normalize(rules[j].watchDir);
                if (Path::Equals(ti, wj) || Path::IsSubPath(wj, ti))
                    return L"规则 " + std::to_wstring(i + 1) + L" 的目标目录落在规则 " +
                           std::to_wstring(j + 1) + L" 的监视目录内: " + ti;
            }
        }
        return L"";
    }

    std::wstring Config::Validate() const
    {
        std::wstring err = ValidateRules();
        if (!err.empty())
            return err;

        for (size_t i = 0; i < rules.size(); ++i)
        {
            if (!rules[i].enabled)
                continue;
            std::wstring w = Path::Normalize(rules[i].watchDir);
            if (!Path::DirectoryExists(w))
                return L"第 " + std::to_wstring(i + 1) + L" 条规则的监视目录不存在: " + w;
        }

        if (workerThreads < 0 || workerThreads > 64)
            return L"线程数必须在 0..64 之间（0 表示自动）";
        if (retryDelayMinutes < 0 || retryDelayMinutes > 1440)
            return L"重试延迟必须在 0..1440 分钟之间（0 表示不自动重试）";

        if (schedule.hour < 0 || schedule.hour > 23)
            return L"定时时刻(小时)必须在 0..23 之间";
        if (schedule.minute < 0 || schedule.minute > 59)
            return L"定时时刻(分钟)必须在 0..59 之间";
        if (schedule.period == SchedulePeriod::Monthly &&
            (schedule.dayOfMonth < 1 || schedule.dayOfMonth > 28))
            return L"每月定时日期必须在 1..28 之间";
        if (schedule.period == SchedulePeriod::EveryNDays &&
            (schedule.everyNDays < 1 || schedule.everyNDays > 365))
            return L"每 N 天定时的 N 必须在 1..365 之间";
        if (schedule.period == SchedulePeriod::Weekly && schedule.weekdayMask == 0)
            return L"每周定时至少要选择一天";
        return L"";
    }

    // ---------------------------------------------------------------------------
    std::wstring ConfigStore::AppConfigDir()
    {
        // 优先 %ProgramData%\dirMover（机器级共享；服务与 GUI 均可访问）
        wchar_t pd[MAX_PATH] = {0};
        DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
        {
            std::wstring dir = Path::Combine(pd, L"dirMover");
            if (FileEngine::EnsureDirectory(dir))
            {
                // 简单可写性测试
                std::wstring probe = Path::Combine(dir, L".wtest");
                if (Path::WriteFileUtf8(probe, L"1"))
                {
                    DeleteFileW(probe.c_str());
                    return dir;
                }
            }
        }
        // 回退到 exe 目录
        return Path::ExeDir();
    }

    static std::wstring g_overridePath;

    void ConfigStore::SetOverridePath(const std::wstring &path)
    {
        g_overridePath = path;
    }

    bool ConfigStore::HasOverridePath()
    {
        return !g_overridePath.empty();
    }

    std::wstring ConfigStore::OverridePath()
    {
        return g_overridePath;
    }

    std::wstring ConfigStore::DefaultPath()
    {
        if (!g_overridePath.empty())
            return g_overridePath;
        return Path::Combine(AppConfigDir(), L"dirmv.ini");
    }

    std::wstring ConfigStore::ConfigPathInDir(const std::wstring &dir)
    {
        return Path::Combine(Path::Combine(dir, L".dirmv"), L"dirmv.ini");
    }

    static std::wstring JoinList(const std::vector<std::wstring> &v)
    {
        std::wstring s;
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i)
                s += L"|";
            s += v[i];
        }
        return s;
    }

    static std::vector<std::wstring> SplitList(const std::wstring &s)
    {
        std::vector<std::wstring> out;
        std::wstring cur;
        for (wchar_t c : s)
        {
            if (c == L'|')
            {
                if (!cur.empty())
                {
                    out.push_back(Path::Trim(cur));
                    cur.clear();
                }
            }
            else
                cur += c;
        }
        if (!cur.empty())
            out.push_back(Path::Trim(cur));
        return out;
    }

    static int StrToInt(const std::wstring &s, int def)
    {
        try
        {
            return std::stoi(s);
        }
        catch (...)
        {
            return def;
        }
    }

    std::wstring SerializeConfig(const Config &c)
    {
        std::wstring text;
        auto sec = [&](const std::wstring &name)
        { text += L"[" + name + L"]\r\n"; };
        auto kv = [&](const wchar_t *k, const std::wstring &v)
        { text += k; text += L"="; text += v; text += L"\r\n"; };
        auto kvi = [&](const wchar_t *k, int v)
        { kv(k, std::to_wstring(v)); };

        for (size_t i = 0; i < c.rules.size(); ++i)
        {
            sec(L"Rule" + std::to_wstring(i + 1));
            kv(L"WatchDir", c.rules[i].watchDir);
            kv(L"TargetDir", c.rules[i].targetDir);
            kv(L"Exclude", JoinList(c.rules[i].exclude));
            kv(L"Migrated", JoinList(c.rules[i].migrated));
            kvi(L"Enabled", c.rules[i].enabled ? 1 : 0);
            kvi(L"PendingDelete", c.rules[i].pendingDelete ? 1 : 0);
            kv(L"PendingDeleteFailed", JoinList(c.rules[i].pendingDeleteFailed));
            text += L"\r\n";
        }

        sec(L"Migrate");
        kvi(L"LinkType", (int)c.linkType);
        kvi(L"MoveMode", c.moveMode ? 1 : 0);
        kvi(L"SkipExisting", c.skipExisting ? 1 : 0);
        kvi(L"WorkerThreads", c.workerThreads);
        kvi(L"EnableWatcher", c.enableWatcher ? 1 : 0);
        text += L"\r\n";

        sec(L"Schedule");
        kvi(L"Period", (int)c.schedule.period);
        kvi(L"Hour", c.schedule.hour);
        kvi(L"Minute", c.schedule.minute);
        kvi(L"WeekdayMask", c.schedule.weekdayMask);
        kvi(L"DayOfMonth", c.schedule.dayOfMonth);
        kvi(L"EveryNDays", c.schedule.everyNDays);
        text += L"\r\n";

        sec(L"Lock");
        kvi(L"Detect", c.lockDetectEnabled ? 1 : 0);
        kvi(L"RetryDelayMinutes", c.retryDelayMinutes);
        kvi(L"Notify", c.notifyOnLock ? 1 : 0);
        text += L"\r\n";

        sec(L"Service");
        kv(L"ServiceName", c.serviceName);
        kvi(L"ServiceThreads", c.serviceThreads);
        text += L"\r\n";

        sec(L"Log");
        kvi(L"LogToFile", c.logToFile ? 1 : 0);
        kvi(L"LogLevel", c.logLevel);
        return text;
    }

    bool ParseConfigText(const std::wstring &content, Config &out)
    {
        out.rules.clear();
        std::wstring section;
        std::vector<Rule> rules;
        Rule curRule;
        bool inRule = false;

        auto flushRule = [&]()
        {
            if (inRule && (!curRule.watchDir.empty() || !curRule.targetDir.empty()))
                rules.push_back(curRule);
            curRule = Rule{};
            inRule = false;
        };

        std::wstring text = content;
        if (!text.empty() && text[0] == 0xFEFF)
            text.erase(0, 1);

        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t eol = text.find(L'\n', pos);
            std::wstring line = (eol == std::wstring::npos) ? text.substr(pos)
                                                            : text.substr(pos, eol - pos);
            pos = (eol == std::wstring::npos) ? text.size() + 1 : eol + 1;
            line = Path::Trim(line);
            if (line.empty() || line[0] == L';' || line[0] == L'#')
                continue;

            if (line.front() == L'[' && line.back() == L']')
            {
                flushRule();
                section = Path::Trim(line.substr(1, line.size() - 2));
                if (section.rfind(L"Rule", 0) == 0)
                    inRule = true;
                continue;
            }

            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos)
                continue;
            std::wstring key = Path::Trim(line.substr(0, eq));
            std::wstring val = Path::Trim(line.substr(eq + 1));

            if (inRule)
            {
                if (key == L"WatchDir")
                    curRule.watchDir = val;
                else if (key == L"TargetDir")
                    curRule.targetDir = val;
                else if (key == L"Exclude")
                    curRule.exclude = SplitList(val);
                else if (key == L"Migrated")
                    curRule.migrated = SplitList(val);
                else if (key == L"Enabled")
                    curRule.enabled = StrToInt(val, 0) != 0;
                else if (key == L"PendingDelete")
                    curRule.pendingDelete = StrToInt(val, 0) != 0;
                else if (key == L"PendingDeleteFailed")
                    curRule.pendingDeleteFailed = SplitList(val);
                continue;
            }

            if (section == L"Migrate")
            {
                if (key == L"LinkType")
                    out.linkType = (LinkType)StrToInt(val, 0);
                else if (key == L"MoveMode")
                    out.moveMode = StrToInt(val, 1) != 0;
                else if (key == L"SkipExisting")
                    out.skipExisting = StrToInt(val, 1) != 0;
                else if (key == L"WorkerThreads")
                    out.workerThreads = StrToInt(val, 4);
                else if (key == L"EnableWatcher")
                    out.enableWatcher = StrToInt(val, 1) != 0;
            }
            else if (section == L"Schedule")
            {
                if (key == L"Period")
                    out.schedule.period = (SchedulePeriod)StrToInt(val, 0);
                else if (key == L"Hour")
                    out.schedule.hour = StrToInt(val, 2);
                else if (key == L"Minute")
                    out.schedule.minute = StrToInt(val, 0);
                else if (key == L"WeekdayMask")
                    out.schedule.weekdayMask = StrToInt(val, 0);
                else if (key == L"DayOfMonth")
                    out.schedule.dayOfMonth = StrToInt(val, 1);
                else if (key == L"EveryNDays")
                    out.schedule.everyNDays = StrToInt(val, 3);
            }
            else if (section == L"Lock")
            {
                if (key == L"Detect")
                    out.lockDetectEnabled = StrToInt(val, 1) != 0;
                else if (key == L"RetryDelayMinutes")
                    out.retryDelayMinutes = StrToInt(val, 5);
                else if (key == L"Notify")
                    out.notifyOnLock = StrToInt(val, 1) != 0;
            }
            else if (section == L"Service")
            {
                if (key == L"ServiceName")
                    out.serviceName = val;
                else if (key == L"ServiceThreads")
                    out.serviceThreads = StrToInt(val, 4);
            }
            else if (section == L"Log")
            {
                if (key == L"LogToFile")
                    out.logToFile = StrToInt(val, 1) != 0;
                else if (key == L"LogLevel")
                    out.logLevel = StrToInt(val, 1);
            }
        }
        flushRule();
        out.rules = std::move(rules);
        return true;
    }

    bool ConfigStore::Load(const std::wstring &path, Config &out)
    {
        out.ApplyDefaults();
        if (!Path::FileExists(path))
            return false;
        std::wstring content = Path::ReadFileUtf8(path);
        if (content.empty())
            return false;

        // 校验签名：篡改时仍尝试加载可用部分，并记录警告
        std::wstring payload;
        int v = Integrity::Verify(content, payload);
        if (v == 1)
        {
            LOGW(L"配置文件无签名: " + path);
            payload = content;
        }
        else if (v == 2)
        {
            LOGE(L"配置文件签名不匹配（可能被篡改）: " + path);
            // 仍以原文加载，由上层提示用户
        }

        ParseConfigText(payload, out);
        if (out.rules.empty())
            out.ApplyDefaults(); // 保底默认规则
        return true;
    }

    bool ConfigStore::SaveMain(const std::wstring &path, const Config &cfg)
    {
        return Path::WriteFileUtf8(path, Integrity::Sign(SerializeConfig(cfg)));
    }

    std::wstring ConfigStore::SerializeSingleRule(const Config &cfg, const Rule &rule)
    {
        // 构造一个只含该规则的临时配置
        Config one = cfg;
        one.rules.clear();
        one.rules.push_back(rule);
        return Integrity::Sign(SerializeConfig(one));
    }

    void ConfigStore::SyncToRuleDirs(const Config &cfg)
    {
        // 收集所有出现过的目录（监视目录 + 目标目录），并记录与每个目录
        // 「相关」的全部规则索引（监视目录或目标目录等于该目录）
        struct DirEntry
        {
            std::wstring norm;
            std::vector<size_t> ruleIdx;
        };
        std::vector<DirEntry> dirs;

        auto addRelated = [&](const std::wstring &dir, size_t idx)
        {
            if (dir.empty() || !Path::DirectoryExists(dir))
                return;
            std::wstring norm = Path::Normalize(dir);
            for (auto &d : dirs)
            {
                if (Path::Equals(d.norm, norm))
                {
                    for (auto k : d.ruleIdx)
                        if (k == idx)
                            return;
                    d.ruleIdx.push_back(idx);
                    return;
                }
            }
            DirEntry e;
            e.norm = norm;
            e.ruleIdx.push_back(idx);
            dirs.push_back(std::move(e));
        };

        for (size_t i = 0; i < cfg.rules.size(); ++i)
        {
            addRelated(cfg.rules[i].watchDir, i);
            addRelated(cfg.rules[i].targetDir, i);
        }

        // 对每个目录，写出与之相关的全部规则（只含相关规则）
        for (auto &d : dirs)
        {
            Config sub = cfg;
            sub.rules.clear();
            for (auto idx : d.ruleIdx)
                sub.rules.push_back(cfg.rules[idx]);

            std::wstring folder = Path::Combine(d.norm, L".dirmv");
            FileEngine::EnsureDirectory(folder);
            std::wstring target = Path::Combine(folder, L"dirmv.ini");
            Path::WriteFileUtf8(target, Integrity::Sign(SerializeConfig(sub)));
        }
    }

    bool ConfigStore::Save(const std::wstring &path, const Config &cfg)
    {
        bool ok = SaveMain(path, cfg);
        SyncToRuleDirs(cfg);
        return ok;
    }

    int ConfigStore::LoadFromDir(const std::wstring &dir, Config &out, bool *tampered)
    {
        if (tampered)
            *tampered = false;
        std::wstring path = ConfigPathInDir(dir);
        if (!Path::FileExists(path))
            return 1; // 不存在
        std::wstring content = Path::ReadFileUtf8(path);
        if (content.empty())
            return 1;

        out.ApplyDefaults();
        std::wstring payload;
        int v = Integrity::Verify(content, payload);
        if (v == 2 && tampered)
            *tampered = true;
        if (v != 0)
            payload = content; // 无签名/被篡改：按原文解析
        ParseConfigText(payload, out);
        return 0;
    }

    ConfigStore::ImportKind ConfigStore::ClassifyDir(const std::wstring &dir)
    {
        std::wstring path = ConfigPathInDir(dir);
        if (!Path::FileExists(path))
            return ImportKind::None;

        Config tmp;
        bool tampered = false;
        if (LoadFromDir(dir, tmp, &tampered) != 0)
            return ImportKind::None;

        std::wstring norm = Path::Normalize(dir);
        bool asWatch = false, asTarget = false;
        for (auto &r : tmp.rules)
        {
            if (!r.watchDir.empty() && Path::Equals(Path::Normalize(r.watchDir), norm))
                asWatch = true;
            if (!r.targetDir.empty() && Path::Equals(Path::Normalize(r.targetDir), norm))
                asTarget = true;
        }
        if (asWatch)
            return ImportKind::WatchConfig;
        if (asTarget)
            return ImportKind::TargetConfig;
        // 都不匹配：说明配置是从别处复制/搬过来的（路径已失效）
        return ImportKind::Relocated;
    }

    std::wstring ConfigStore::ImportFromDir(const std::wstring &dir,
                                            std::vector<Rule> &outRules,
                                            ImportKind &kind, bool &tampered,
                                            bool &watchMatched)
    {
        outRules.clear();
        kind = ImportKind::None;
        tampered = false;
        watchMatched = false;

        if (dir.empty() || !Path::DirectoryExists(dir))
            return L"目录不存在: " + dir;

        std::wstring path = ConfigPathInDir(dir);
        if (!Path::FileExists(path))
            return L"该目录下没有配置文件: " + path;

        Config tmp;
        if (LoadFromDir(dir, tmp, &tampered) != 0)
            return L"读取配置失败: " + path;
        if (tmp.rules.empty())
            return L"配置中没有规则: " + path;

        outRules = tmp.rules;

        std::wstring norm = Path::Normalize(dir);
        bool asWatch = false, asTarget = false;
        for (auto &r : tmp.rules)
        {
            if (!r.watchDir.empty() && Path::Equals(Path::Normalize(r.watchDir), norm))
                asWatch = true;
            if (!r.targetDir.empty() && Path::Equals(Path::Normalize(r.targetDir), norm))
                asTarget = true;
        }
        kind = asWatch ? ImportKind::WatchConfig
                       : (asTarget ? ImportKind::TargetConfig : ImportKind::Relocated);

        // 监视目录是否与所选目录一致（目标目录配置通常不一致）
        watchMatched = asWatch;
        return L"";
    }

} // namespace dirmv
