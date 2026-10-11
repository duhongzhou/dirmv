#pragma once
// ---------------------------------------------------------------------------
// Scheduler.h - 定时调度计算 + 一次性重试计划任务
//   · ComputeNextRun: 根据 Schedule(周期+时刻) 计算下一次运行时间
//   · CreateOnceRetryTask: 使用 schtasks.exe 创建「一次性」重试任务
//     （占用重试：只创建一次；若任务已存在则不再创建）
// ---------------------------------------------------------------------------
#include "core/Config.h"

#include <string>
#include <ctime>

namespace dirmv
{

    struct NextRun
    {
        bool valid = false;
        std::time_t when = 0; // 本地时间
    };

    class Scheduler
    {
    public:
        // 计算下一次运行时间（基于本地时间 now）
        static NextRun ComputeNextRun(const Schedule &s, std::time_t now);

        // 创建一次性重试计划任务（在 delayMinutes 分钟后运行一次本程序迁移）。
        // 若同名任务已存在则不重复创建（保证「只创建一次」）。
        // 命令形式：<exe> --cli migrate --config <ini>
        // 返回空串=成功，否则为错误信息。
        static std::wstring CreateOnceRetryTask(const std::wstring &taskName,
                                                const std::wstring &exePath,
                                                const std::wstring &configPath,
                                                int delayMinutes);

        static bool DeleteTask(const std::wstring &taskName);

        // 计划任务是否已存在
        static bool TaskExists(const std::wstring &taskName);
    };

} // namespace dirmv
