#include "core/Scheduler.h"
#include "core/PathUtil.h"
#include "core/Log.h"

#include <windows.h>
#include <vector>
#include <cstdio>

namespace dirmv
{

    NextRun Scheduler::ComputeNextRun(const Schedule &s, std::time_t now)
    {
        NextRun r;
        if (s.period == SchedulePeriod::Disabled)
            return r;

        std::tm local{};
        localtime_s(&local, &now);
        int year = local.tm_year + 1900;
        int mon = local.tm_mon + 1;
        int day = local.tm_mday;

        auto candidate = [&](int addDays) -> std::time_t
        {
            std::tm d = local;
            d.tm_year = year - 1900;
            d.tm_mon = mon - 1;
            d.tm_mday = day;
            d.tm_hour = s.hour;
            d.tm_min = s.minute;
            d.tm_sec = 0;
            d.tm_isdst = -1;
            std::time_t base = std::mktime(&d);
            return base + (std::time_t)addDays * 86400;
        };

        switch (s.period)
        {
        case SchedulePeriod::Daily:
        {
            std::time_t t = candidate(0);
            if (t <= now)
                t = candidate(1);
            r.when = t;
            r.valid = true;
            break;
        }
        case SchedulePeriod::EveryNDays:
        {
            int n = s.everyNDays < 1 ? 1 : s.everyNDays;
            std::time_t t = candidate(0);
            if (t <= now)
                t = candidate(n);
            r.when = t;
            r.valid = true;
            break;
        }
        case SchedulePeriod::Weekly:
        {
            if (s.weekdayMask == 0)
                break;
            // 找未来 0..7 天内第一个匹配的星期几
            for (int d = 0; d <= 7; ++d)
            {
                std::time_t t = candidate(d);
                if (t <= now)
                    continue;
                std::tm tt{};
                localtime_s(&tt, &t);
                int wd = tt.tm_wday; // 0=周日
                if (s.weekdayMask & (1 << wd))
                {
                    r.when = t;
                    r.valid = true;
                    break;
                }
            }
            break;
        }
        case SchedulePeriod::Monthly:
        {
            int dom = s.dayOfMonth < 1 ? 1 : (s.dayOfMonth > 28 ? 28 : s.dayOfMonth);
            std::tm d = local;
            d.tm_hour = s.hour;
            d.tm_min = s.minute;
            d.tm_sec = 0;
            d.tm_isdst = -1;
            d.tm_mday = dom;
            std::time_t t = std::mktime(&d);
            if (t <= now)
            {
                // 下个月
                std::tm nd = local;
                nd.tm_mon += 1;
                nd.tm_mday = 1;
                nd.tm_hour = s.hour;
                nd.tm_min = s.minute;
                nd.tm_sec = 0;
                nd.tm_isdst = -1;
                std::time_t nm = std::mktime(&nd);
                std::tm nmt{};
                localtime_s(&nmt, &nm);
                nmt.tm_mday = dom;
                nmt.tm_hour = s.hour;
                nmt.tm_min = s.minute;
                t = std::mktime(&nmt);
            }
            r.when = t;
            r.valid = true;
            break;
        }
        default:
            break;
        }
        return r;
    }

    // ---------------------------------------------------------------------------
    // 计划任务（schtasks.exe）
    // ---------------------------------------------------------------------------
    static std::wstring RunAndCapture(const std::wstring &cmdLine, DWORD &exitCode)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE rPipe = nullptr, wPipe = nullptr;
        if (!CreatePipe(&rPipe, &wPipe, &sa, 0))
            return L"";
        SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = wPipe;
        si.hStdError = wPipe;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
        buf.push_back(L'\0');

        std::wstring output;
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(wPipe);
            char tmp[4096];
            DWORD read = 0;
            while (ReadFile(rPipe, tmp, sizeof(tmp) - 1, &read, nullptr) && read > 0)
            {
                tmp[read] = 0;
                int wlen = MultiByteToWideChar(CP_OEMCP, 0, tmp, (int)read, nullptr, 0);
                std::wstring w(wlen, L'\0');
                MultiByteToWideChar(CP_OEMCP, 0, tmp, (int)read, &w[0], wlen);
                output += w;
            }
            WaitForSingleObject(pi.hProcess, 30000);
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        else
        {
            exitCode = (DWORD)-1;
            CloseHandle(wPipe);
        }
        CloseHandle(rPipe);
        return output;
    }

    bool Scheduler::TaskExists(const std::wstring &taskName)
    {
        DWORD ec = 0;
        std::wstring out = RunAndCapture(
            L"schtasks.exe /Query /TN \"" + taskName + L"\"", ec);
        return ec == 0;
    }

    std::wstring Scheduler::CreateOnceRetryTask(const std::wstring &taskName,
                                                const std::wstring &exePath,
                                                const std::wstring &configPath,
                                                int delayMinutes)
    {
        // 重新武装：任务可能已经执行过（一次性任务执行后仍留在计划任务库里），
        // 若直接跳过就再也不会重试。这里统一用 /F 覆盖重建。
        if (delayMinutes < 1)
            delayMinutes = 5;

        // 计算触发时刻 = 现在 + delayMinutes
        std::time_t now = time(nullptr);
        std::time_t when = now + (std::time_t)delayMinutes * 60;
        std::tm lt{};
        localtime_s(&lt, &when);
        wchar_t timeStr[16];
        swprintf_s(timeStr, L"%02d:%02d", lt.tm_hour, lt.tm_min);
        wchar_t dateStr[16];
        // schtasks /SD 需要 yyyy/mm/dd 格式
        swprintf_s(dateStr, L"%04d/%02d/%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);

        // 优先使用同目录下的控制台版 dirMover_cli.exe（GUI 版 --cli 也可，但控制台版更合适）
        std::wstring exe = exePath;
        std::wstring cliExe = Path::Combine(Path::ParentDir(exePath), L"dirMover_cli.exe");
        bool useCli = Path::FileExists(cliExe);
        if (useCli)
            exe = cliExe;

        // 控制台版直接跟 migrate；GUI 版需要 --cli 开关
        std::wstring cmd = L"\\\"" + exe + L"\\\"" + (useCli ? L"" : L" --cli") +
                           L" migrate --config \\\"" + configPath + L"\\\"";

        // /RU SYSTEM：以 SYSTEM 运行，不依赖当前登录用户；/RL HIGHEST 确保
        // 迁移时能创建符号链接；/F 覆盖同名旧任务，保证每次失败都能重新武装。
        std::wstring full = L"schtasks.exe /Create /F /TN \"" + taskName +
                            L"\" /TR \"" + cmd + L"\" /SC ONCE /SD " + dateStr +
                            L" /ST " + timeStr + L" /RL HIGHEST /RU SYSTEM";
        DWORD ec = 0;
        std::wstring out = RunAndCapture(full, ec);
        if (ec != 0)
        {
            LOGE(L"创建重试计划任务失败: " + out);
            return L"创建重试计划任务失败 (" + std::to_wstring(ec) + L")";
        }
        LOGI(L"已创建一次性重试计划任务: " + taskName + L" 于 " +
             dateStr + L" " + timeStr + L"（以 SYSTEM 运行）");
        return L"";
    }

    bool Scheduler::DeleteTask(const std::wstring &taskName)
    {
        if (!TaskExists(taskName))
            return true;
        DWORD ec = 0;
        RunAndCapture(L"schtasks.exe /Delete /F /TN \"" + taskName + L"\"", ec);
        return ec == 0;
    }

} // namespace dirmv
