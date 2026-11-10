// ---------------------------------------------------------------------------
// CliRunner.cpp - 控制台命令实现（放入核心库，供 GUI 的 --cli 分支复用）
// ---------------------------------------------------------------------------
#include "core/CliRunner.h"
#include "core/Config.h"
#include "core/MigrateEngine.h"
#include "core/PathUtil.h"
#include "core/Log.h"
#include "core/PipeServer.h"
#include "core/Scheduler.h"
#include "service/ServiceMain.h"

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>

namespace dirmv
{

    // 向 stdout 输出宽字符串：控制台用 WriteConsoleW，重定向用 UTF-8 字节
    static void WOut(const std::wstring &s)
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode))
        {
            DWORD written = 0;
            WriteConsoleW(hOut, s.c_str(), (DWORD)s.size(), &written, nullptr);
        }
        else
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                          nullptr, 0, nullptr, nullptr);
            std::string utf8((size_t)len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                &utf8[0], len, nullptr, nullptr);
            fwrite(utf8.data(), 1, utf8.size(), stdout);
            fflush(stdout);
        }
    }

    static void PrintUsage()
    {
        WOut(L"dirMover 命令行用法：\n"
             L"  migrate     执行一次迁移\n"
             L"  restore     从日志恢复\n"
             L"  recover [--dry-run] [--index N]  修复崩溃残留(.dirmv-moving/.dirmv-old)\n"
             L"  scan        列出待迁移目录\n"
             L"  change-target --index N --newtarget <目录>   更换某规则目标目录\n"
             L"  restore-excluded --index N --name <目录名>   还原（加入白名单前）\n"
             L"  exclude --index N --name <目录名>            加入白名单(若已迁移会先还原)\n"
             L"  unexclude --index N --name <目录名>          从白名单移除\n"
             L"  adopt --index N                              扫描接管已有迁移/链接\n"
             L"  rebuild --index N --watch <新监视目录>       新机器上重建链接\n"
             L"  delete-rule --index N                        删除规则(先还原再删除)\n"
             L"  import --dir <目录> [--kind watch|target|full] [--watch <新监视目录>] [--target <新目标目录>]  导入规则(从目录的配置)\n"
             L"  install     安装并开机自启动服务\n"
             L"  uninstall   卸载服务\n"
             L"  start       启动服务\n"
             L"  stop        停止服务\n"
             L"  status      查询服务状态\n"
             L"  （全局参数）--config <path>  指定配置文件\n");
    }

    static std::vector<std::wstring> SplitArgs(const std::wstring &cmd)
    {
        std::vector<std::wstring> out;
        std::wstring cur;
        bool inQuote = false;
        for (wchar_t c : cmd)
        {
            if (c == L'"')
            {
                inQuote = !inQuote;
                continue;
            }
            if (!inQuote && (c == L' ' || c == L'\t'))
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
            }
            else
                cur += c;
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    static void PrintProgress(const ProgressInfo &pi)
    {
        wchar_t buf[512];
        if (pi.etaSeconds >= 0)
        {
            int sec = (int)pi.etaSeconds;
            swprintf_s(buf, L"\r[%s] %.1f%%  %d/%d  剩余 %d:%02d:%02d  %s      ",
                       pi.phase.c_str(), pi.percent, pi.doneItems, pi.totalItems,
                       sec / 3600, (sec % 3600) / 60, sec % 60, pi.currentItem.c_str());
        }
        else
        {
            swprintf_s(buf, L"\r[%s] %.1f%%  %d/%d  %s      ",
                       pi.phase.c_str(), pi.percent, pi.doneItems, pi.totalItems,
                       pi.currentItem.c_str());
        }
        WOut(buf);
    }

    int RunCliCommand(const std::vector<std::wstring> &args)
    {
        std::wstring configPath = ConfigStore::DefaultPath();
        std::wstring cmd;
        int ruleIndex = -1;
        std::wstring newTarget, excludeName, watchDir, importDir, importTarget, importKind;
        bool dryRun = false;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == L"--dry-run")
                dryRun = true;
            else if (args[i] == L"--config" && i + 1 < args.size())
                configPath = args[++i];
            else if (args[i] == L"--index" && i + 1 < args.size())
                ruleIndex = _wtoi(args[++i].c_str());
            else if (args[i] == L"--newtarget" && i + 1 < args.size())
                newTarget = args[++i];
            else if (args[i] == L"--watch" && i + 1 < args.size())
                watchDir = args[++i];
            else if (args[i] == L"--dir" && i + 1 < args.size())
                importDir = args[++i];
            else if (args[i] == L"--target" && i + 1 < args.size())
                importTarget = args[++i];
            else if (args[i] == L"--kind" && i + 1 < args.size())
                importKind = args[++i];
            else if (args[i] == L"--name" && i + 1 < args.size())
                excludeName = args[++i];
            else if (cmd.empty())
                cmd = args[i];
        }

        bool isServiceCmd = (cmd == L"install" || cmd == L"uninstall" ||
                             cmd == L"start" || cmd == L"stop" || cmd == L"status");

        Config cfg;
        ConfigStore::Load(configPath, cfg);
        std::wstring svcName = cfg.serviceName.empty() ? L"dirMoverSvc" : cfg.serviceName;

        // 同步配置到三处（应用目录 / 监视目录 / 目标目录）
        ConfigStore::SyncToRuleDirs(cfg);

        if (isServiceCmd)
        {
            std::wstring err;
            if (cmd == L"install")
            {
                bool ok = ServiceInstaller::Install(svcName, L"dirMover 目录迁移服务", &err);
                WOut(ok ? L"服务已安装（开机自动启动）\n" : L"安装失败: " + err + L"\n");
                return ok ? 0 : 1;
            }
            if (cmd == L"uninstall")
            {
                bool ok = ServiceInstaller::Uninstall(svcName, &err);
                WOut(ok ? L"服务已卸载\n" : L"卸载失败: " + err + L"\n");
                return ok ? 0 : 1;
            }
            if (cmd == L"start")
            {
                bool ok = ServiceInstaller::Start(svcName, &err);
                WOut(ok ? L"服务已启动\n" : L"启动失败: " + err + L"\n");
                return ok ? 0 : 1;
            }
            if (cmd == L"stop")
            {
                bool ok = ServiceInstaller::Stop(svcName, &err);
                WOut(ok ? L"服务已停止\n" : L"停止失败: " + err + L"\n");
                return ok ? 0 : 1;
            }
            if (cmd == L"status")
            {
                int st = ServiceInstaller::QueryState(svcName);
                const wchar_t *t = (st == 1) ? L"运行中" : (st == 0) ? L"已停止"
                                                       : (st == 2)   ? L"状态切换中"
                                                                     : L"未知/需管理员权限";
                WOut(std::wstring(L"服务状态: ") + t + L"\n");
                return 0;
            }
        }

        // migrate / import / exclude：不做整份配置校验，交由命令逐条处理可用规则
        bool perRuleCmd = (cmd == L"migrate" || cmd == L"import" || cmd == L"recover" ||
                           cmd == L"exclude" || cmd == L"unexclude");
        if (!perRuleCmd)
        {
            std::wstring verr = cfg.Validate();
            if (!verr.empty())
            {
                WOut(L"配置无效: " + verr + L"\n");
                return 1;
            }
        }

        MigrateEngine engine(cfg);
        if (cmd == L"scan")
        {
            int total = 0;
            for (auto &rule : cfg.rules)
            {
                if (!rule.enabled)
                    continue;
                auto pend = engine.ScanPending(rule);
                WOut(L"规则 [" + rule.watchDir + L"] → [" + rule.targetDir +
                     L"] 待迁移 " + std::to_wstring(pend.size()) + L" 个：\n");
                for (auto &n : pend)
                    WOut(L"  " + n + L"\n");
                total += (int)pend.size();
            }
            WOut(L"合计待迁移目录 " + std::to_wstring(total) + L" 个\n");
            return 0;
        }
        if (cmd == L"migrate")
        {
            std::atomic<bool> cancel{false};
            auto r = engine.Migrate(
                PrintProgress, &cancel,
                [](const LockNotice &n)
                {
                    WOut(L"\n[占用] 子任务 " + n.subtask + L" 有文件被占用：\n");
                    for (auto &l : n.locks)
                    {
                        std::wstring line = L"        " + Path::FileName(l.filePath);
                        if (!l.processes.empty())
                        {
                            line += L"  ← ";
                            for (size_t i = 0; i < l.processes.size(); ++i)
                            {
                                if (i)
                                    line += L", ";
                                line += l.processes[i];
                            }
                        }
                        WOut(line + L"\n");
                    }
                });
            WOut(L"\n完成：处理 " + std::to_wstring(r.total) + L"，成功 " +
                 std::to_wstring(r.migrated) + L"，跳过 " + std::to_wstring(r.skipped) +
                 L"，占用 " + std::to_wstring(r.locked) +
                 L"，失败 " + std::to_wstring(r.failed) + L"\n");

            // 若有占用/失败，创建一次性重试任务（只创建一次）
            if ((r.locked > 0 || r.failed > 0) && cfg.retryDelayMinutes > 0)
            {
                std::wstring taskName = L"dirMover_RetryOnce";
                std::wstring err = Scheduler::CreateOnceRetryTask(
                    taskName, Path::ExePath(), configPath, cfg.retryDelayMinutes);
                if (err.empty())
                    WOut(L"已创建一次性重试任务，将在 " +
                         std::to_wstring(cfg.retryDelayMinutes) + L" 分钟后重试。\n");
                else
                    WOut(L"重试任务: " + err + L"\n");
            }
            return (r.failed || r.locked) ? 2 : 0;
        }
        if (cmd == L"recover")
        {
            MigrateEngine::RecoverReport sum;
            int picked = 0;
            for (size_t i = 0; i < cfg.rules.size(); ++i)
            {
                if (!cfg.rules[i].enabled)
                    continue;
                if (ruleIndex >= 0 && (int)i != ruleIndex)
                    continue;
                picked++;
                auto r = engine.RecoverRule(cfg.rules[i], dryRun);
                for (auto &it : r.items)
                {
                    const wchar_t *tag = it.kind == 1 ? L"[补链接]"
                                         : it.kind == 2 ? L"[还原]  "
                                                        : it.kind == 3 ? L"[清理]  "
                                                                       : L"[保留]  ";
                    WOut(std::wstring(L"  ") + tag + L" " + it.path + L"  — " +
                         it.action + L"\n");
                }
                sum.scanned += r.scanned;
                sum.relinked += r.relinked;
                sum.restored += r.restored;
                sum.cleaned += r.cleaned;
                sum.kept += r.kept;
            }
            if (picked == 0)
            {
                WOut(L"没有可处理的启用规则\n");
                return 1;
            }
            WOut(std::wstring(dryRun ? L"\n预演（未修改磁盘）：" : L"\n恢复完成：") +
                 L"扫描残留 " + std::to_wstring(sum.scanned) + L"，补建链接 " +
                 std::to_wstring(sum.relinked) + L"，还原原名 " +
                 std::to_wstring(sum.restored) + L"，清理重复 " +
                 std::to_wstring(sum.cleaned) + L"，保留待查 " +
                 std::to_wstring(sum.kept) + L"\n");
            return 0;
        }
        if (cmd == L"restore")
        {
            std::atomic<bool> cancel{false};
            auto r = engine.Restore(PrintProgress, &cancel);
            WOut(L"\n恢复完成：成功 " + std::to_wstring(r.migrated) +
                 L"，失败 " + std::to_wstring(r.failed) + L"\n");
            return r.failed ? 2 : 0;
        }
        if (cmd == L"change-target")
        {
            if (ruleIndex < 0 || ruleIndex >= (int)cfg.rules.size())
            {
                WOut(L"用法: change-target --index <N> --newtarget <目录> --config <ini>\n");
                return 1;
            }
            if (newTarget.empty())
            {
                WOut(L"缺少 --newtarget\n");
                return 1;
            }
            std::atomic<bool> cancel{false};
            std::wstring err2 = engine.ChangeTargetDir(cfg.rules[ruleIndex], newTarget,
                                                       PrintProgress, &cancel);
            if (err2.empty())
            {
                cfg.rules[ruleIndex].targetDir = Path::Normalize(newTarget);
                ConfigStore::Save(configPath, cfg);
                WOut(L"\n更换目标目录完成: " + cfg.rules[ruleIndex].targetDir + L"\n");
                return 0;
            }
            WOut(L"\n更换目标目录失败: " + err2 + L"\n");
            return 2;
        }
        if (cmd == L"restore-excluded")
        {
            if (ruleIndex < 0 || ruleIndex >= (int)cfg.rules.size())
            {
                WOut(L"用法: restore-excluded --index <N> --name <目录名> --config <ini>\n");
                return 1;
            }
            if (excludeName.empty())
            {
                WOut(L"缺少 --name\n");
                return 1;
            }
            std::atomic<bool> cancel{false};
            std::wstring err2 = engine.RestoreExcluded(cfg.rules[ruleIndex], excludeName, &cancel);
            if (err2.empty())
            {
                WOut(L"\n已还原（若原本已迁移）: " + excludeName + L"\n");
                return 0;
            }
            WOut(L"\n还原失败: " + err2 + L"\n");
            return 2;
        }
        if (cmd == L"exclude" || cmd == L"unexclude")
        {
            if (ruleIndex < 0 || ruleIndex >= (int)cfg.rules.size())
            {
                WOut(L"用法: exclude|unexclude --index <N> --name <目录名> [--config <ini>]\n");
                return 1;
            }
            if (excludeName.empty())
            {
                WOut(L"缺少 --name\n");
                return 1;
            }
            Rule &rule = cfg.rules[ruleIndex];
            bool adding = (cmd == L"exclude");
            if (adding)
            {
                bool exists = false;
                for (auto &e : rule.exclude)
                    if (Path::Equals(e, excludeName))
                    {
                        exists = true;
                        break;
                    }
                if (exists)
                {
                    WOut(L"该目录已在白名单中: " + excludeName + L"\n");
                    return 0;
                }
                // 若该目录已被迁移（源是链接），先还原再进白名单，避免留下链接
                std::atomic<bool> cancel{false};
                std::wstring rerr = engine.RestoreExcluded(rule, excludeName, &cancel);
                if (!rerr.empty())
                {
                    WOut(L"还原失败，未修改配置: " + rerr + L"\n");
                    return 2;
                }
                rule.exclude.push_back(excludeName);
            }
            else
            {
                bool removed = false;
                for (auto it = rule.exclude.begin(); it != rule.exclude.end(); ++it)
                    if (Path::Equals(*it, excludeName))
                    {
                        rule.exclude.erase(it);
                        removed = true;
                        break;
                    }
                if (!removed)
                {
                    WOut(L"该目录不在白名单中: " + excludeName + L"\n");
                    return 0;
                }
            }
            if (!ConfigStore::Save(configPath, cfg))
            {
                WOut(L"保存配置失败\n");
                return 2;
            }
            WOut(adding ? L"已保存：" + excludeName + L" 已加入白名单\n"
                        : L"已保存：" + excludeName + L" 已从白名单移除\n");
            return 0;
        }
        if (cmd == L"adopt")
        {
            // 扫描接管：把已有的链接/目标文件夹补全进配置
            if (ruleIndex < 0 || ruleIndex >= (int)cfg.rules.size())
            {
                WOut(L"用法: adopt --index <N> --config <ini>\n");
                return 1;
            }
            AdoptResult ar = engine.AdoptExisting(cfg.rules[ruleIndex]);
            WOut(L"\n接管扫描结果：\n");
            WOut(L"  新增纳入 " + std::to_wstring(ar.adopted.size()) + L" 个：\n");
            for (auto &n : ar.adopted)
                WOut(L"    + " + n + L"\n");
            WOut(L"  普通目录 " + std::to_wstring(ar.plainDirs.size()) + L" 个\n");
            if (!ar.foreignLinks.empty())
            {
                WOut(L"  [提醒] 以下链接指向的不是本规则的目标目录：\n");
                for (auto &f : ar.foreignLinks)
                    WOut(L"    ! " + f.linkPath + L"  ->  " + f.target + L"\n");
            }
            ConfigStore::Save(configPath, cfg);
            WOut(L"已保存配置。\n");
            return 0;
        }
        if (cmd == L"import")
        {
            // 从某目录的 .dirmv\dirmv.ini 导入规则
            if (importDir.empty())
            {
                WOut(L"用法: import --dir <目录> --config <ini> [--kind watch|target|full] "
                     L"[--watch <新监视目录>] [--target <新目标目录>]\n"
                     L"  --kind watch : 本目录是监视目录（逐条用 --target 指定目标目录）\n"
                     L"  --kind target: 本目录是目标目录（逐条用 --watch 指定监视目录）\n"
                     L"  --kind full  : 完整多规则配置（本机不存在的监视/目标目录将剔除）\n"
                     L"  不指定 --kind 时按自动识别结果处理\n");
                return 1;
            }
            std::vector<Rule> rules;
            ConfigStore::ImportKind kind = ConfigStore::ImportKind::None;
            bool tampered = false, watchMatched = false;
            std::wstring err = ConfigStore::ImportFromDir(importDir, rules, kind, tampered, watchMatched);
            if (!err.empty())
            {
                WOut(L"导入失败: " + err + L"\n");
                return 1;
            }
            const wchar_t *kindText =
                (kind == ConfigStore::ImportKind::WatchConfig)    ? L"监视目录配置"
                : (kind == ConfigStore::ImportKind::TargetConfig) ? L"目标目录配置"
                : (kind == ConfigStore::ImportKind::Relocated)    ? L"复制/搬迁的目录配置"
                                                                  : L"未知来源";
            WOut(L"来源: " + importDir + L"  (" + kindText + L")\n");
            if (tampered)
                WOut(L"[警告] 签名校验不通过（配置可能被改动）\n");

            // 应用 --kind 指定的类型
            bool asFull = false;
            if (importKind == L"watch")
            {
                for (auto &r : rules)
                    r.watchDir = Path::Normalize(importDir);
                watchMatched = true;
                WOut(L"类型: 监视目录配置（监视目录=本目录）\n");
            }
            else if (importKind == L"target")
            {
                for (auto &r : rules)
                    r.targetDir = Path::Normalize(importDir);
                watchMatched = false;
                WOut(L"类型: 目标目录配置（目标目录=本目录）\n");
            }
            else if (importKind == L"full")
            {
                asFull = true;
                watchMatched = false;
                WOut(L"类型: 完整配置(多规则)\n");
            }
            else
            {
                // 自动识别：搬迁场景用 --target 替换目标目录
                if (kind == ConfigStore::ImportKind::Relocated)
                {
                    if (!importTarget.empty())
                    {
                        std::wstring nt = Path::Normalize(importTarget);
                        for (auto &r : rules)
                            r.targetDir = nt;
                        watchMatched = false;
                        WOut(L"已将目标目录替换为: " + nt + L"\n");
                    }
                    else
                    {
                        WOut(L"[提示] 该配置路径已失效（如从别处复制）。\n"
                             L"       可用 --kind target --target <目录> 指定目标目录。\n");
                    }
                }
            }

            int imported = 0, skipped = 0;
            for (auto &r : rules)
            {
                // 完整配置：剔除本机不存在的监视/目标目录
                if (asFull)
                {
                    std::wstring why;
                    if (!Path::DirectoryExists(r.watchDir))
                        why = L"监视目录不存在";
                    else if (r.targetDir.empty() || !Path::DirectoryExists(r.targetDir))
                        why = L"目标目录不存在";
                    if (!why.empty())
                    {
                        WOut(L"[剔除] " + r.watchDir + L" -> " + r.targetDir + L"  (" + why + L")\n");
                        skipped++;
                        continue;
                    }
                }
                else
                {
                    // 监视目录不匹配：用 --watch 指定的新监视目录替换
                    if (!watchMatched || !Path::DirectoryExists(r.watchDir))
                    {
                        if (!watchDir.empty())
                            r.watchDir = Path::Normalize(watchDir);
                        else if (!Path::DirectoryExists(r.watchDir))
                        {
                            WOut(L"[跳过] 监视目录不可用(可用 --watch 重新指定): " + r.watchDir + L"\n");
                            skipped++;
                            continue;
                        }
                    }
                    // 目标目录缺失/不存在：用 --target 指定
                    if (r.targetDir.empty() || !Path::DirectoryExists(r.targetDir))
                    {
                        if (!importTarget.empty())
                            r.targetDir = Path::Normalize(importTarget);
                        else
                        {
                            WOut(L"[跳过] 目标目录不可用(可用 --target 重新指定): " +
                                 (r.targetDir.empty() ? L"(未设置)" : r.targetDir) + L"\n");
                            skipped++;
                            continue;
                        }
                    }
                }

                bool dup = false;
                for (auto &e : cfg.rules)
                    if (Path::Equals(e.watchDir, r.watchDir) &&
                        Path::Equals(e.targetDir, r.targetDir))
                    {
                        dup = true;
                        break;
                    }
                if (dup)
                {
                    skipped++;
                    continue;
                }
                cfg.rules.push_back(r);
                imported++;
            }
            ConfigStore::Save(configPath, cfg);
            WOut(L"导入完成，共 " + std::to_wstring(imported) + L" 条规则" +
                 (skipped > 0 ? L"（跳过/剔除 " + std::to_wstring(skipped) + L" 条）" : L"") + L"。\n");
            return imported > 0 ? 0 : 2;
        }
        if (cmd == L"delete-rule")
        {
            // 删除规则：先还原全部已迁移目录，全部成功才删除；
            // 有失败则标记为待删除并保留（记录失败目录）
            if (ruleIndex < 0 || ruleIndex >= (int)cfg.rules.size())
            {
                WOut(L"用法: delete-rule --index <N> --config <ini>\n");
                return 1;
            }
            Rule ruleCopy = cfg.rules[ruleIndex];
            bool resume = ruleCopy.pendingDelete && !ruleCopy.pendingDeleteFailed.empty();
            std::vector<std::wstring> onlyDirs = resume ? ruleCopy.pendingDeleteFailed
                                                        : std::vector<std::wstring>{};
            std::atomic<bool> cancel{false};
            auto rep = engine.RestoreRuleDetailed(ruleCopy, onlyDirs, PrintProgress, &cancel);
            WOut(L"\n还原：成功 " + std::to_wstring(rep.succeeded) +
                 L"，跳过 " + std::to_wstring(rep.skipped) +
                 L"，失败 " + std::to_wstring(rep.failedDirs.size()) + L"\n");

            if (rep.failedDirs.empty())
            {
                cfg.rules.erase(cfg.rules.begin() + ruleIndex);
                ConfigStore::Save(configPath, cfg);
                WOut(L"全部还原成功，规则已删除。\n");
                return 0;
            }
            cfg.rules[ruleIndex].pendingDelete = true;
            cfg.rules[ruleIndex].pendingDeleteFailed = rep.failedDirs;
            ConfigStore::Save(configPath, cfg);
            WOut(L"以下目录还原失败，规则已标记为待删除：\n");
            for (auto &d : rep.failedDirs)
                WOut(L"  • " + d + L"\n");
            return 2;
        }
        if (cmd == L"rebuild")
        {
            // 在新机器上从目标目录配置重建链接
            if (ruleIndex < 0 || ruleIndex >= (int)cfg.rules.size())
            {
                WOut(L"用法: rebuild --index <N> --watch <新监视目录> --config <ini>\n");
                return 1;
            }
            if (watchDir.empty())
            {
                WOut(L"缺少 --watch <新监视目录>\n");
                return 1;
            }
            std::atomic<bool> cancel{false};
            std::wstring err2 = engine.RebuildLinksFromTarget(cfg.rules[ruleIndex],
                                                              watchDir, PrintProgress, &cancel);
            if (err2.empty())
            {
                cfg.rules[ruleIndex].watchDir = Path::Normalize(watchDir);
                ConfigStore::Save(configPath, cfg);
                WOut(L"\n重建链接完成（监视目录: " + cfg.rules[ruleIndex].watchDir + L"）\n");
                return 0;
            }
            WOut(L"\n重建链接失败: " + err2 + L"\n");
            return 2;
        }

        PrintUsage();
        return 1;
    }

    int RunCli(const std::wstring &cmdLine)
    {
        SetConsoleOutputCP(CP_UTF8);
        Logger::Instance().EnableConsole(true);
        Logger::Instance().SetFile(Path::Combine(Path::ExeDir(), L"dirmv_cli.log"));
        std::vector<std::wstring> args = SplitArgs(cmdLine);
        return RunCliCommand(args);
    }

} // namespace dirmv
