#pragma once
// ---------------------------------------------------------------------------
// MainWindow.h - 主界面（分页：规则 / 计划 / 关于）
//   规则页：规则列表 + 添加/修改/删除（全部通过按钮流程，不允许手改）
//   计划页：定时调度、线程、链接类型、占用重试
//   关于页：版本、说明
// ---------------------------------------------------------------------------
#include <windows.h>
#include "core/Config.h"

namespace dirmv::gui
{

    class MainWindow
    {
    public:
        static void Register(HINSTANCE hInst);
        static HWND Create(HINSTANCE hInst, int nCmdShow);

        enum : int
        {
            // Tab
            IDC_TAB = 1000,

            // 规则页
            IDC_RULE_LIST = 1001, // ListView：全选 | 监控目录 | 目标目录 | 白名单数 | 状态 | 操作(编辑)
            IDC_BTN_ADD_RULE = 1002,
            IDC_BTN_EDIT_RULE = 1003, // 弹出窗口修改规则（点击列表「操作」列触发）
            IDC_BTN_DELETE_RULE = 1006,
            IDC_BTN_MIGRATE_RULE = 1007,
            IDC_BTN_RESTORE_RULE = 1008,
            IDC_BTN_ENABLE_RULE = 1009,  // 启用（分开的按钮）
            IDC_BTN_DISABLE_RULE = 1016, // 停用（分开的按钮）
            IDC_BTN_ADOPT = 1010,        // 扫描已有迁移
            IDC_BTN_IMPORT = 1015,       // 导入规则（从目录的配置文件）
            IDC_BTN_REBUILD = 1014,      // 新机器重建链接

            // 共享
            IDC_LOG = 1011,
            IDC_PROGRESS = 1012,
            IDC_ETA = 1013,
            IDC_BTN_STOP = 1022,

            // 计划页
            IDC_SCHED_PERIOD = 1020,
            IDC_SCHED_HOUR = 1021,
            IDC_SCHED_MIN = 1023,
            IDC_SCHED_DOM = 1024,
            IDC_SCHED_NEXT = 1025,
            IDC_SCHED_WEEK = 1033, // 每周：周几下拉
            IDC_SCHED_N = 1035,    // 每N天：N
            IDC_THREADS = 1026,
            IDC_LINKTYPE = 1027,
            IDC_WATCHER = 1028,
            IDC_MOVEMODE = 1029,
            IDC_LOCKDETECT = 1030,
            IDC_RETRYMIN = 1031,
            IDC_BTN_SAVE_PLAN = 1032,

            // 关于页 + 服务
            IDC_ABOUT = 1040,
            IDC_SVCSTATE = 1041,
            IDC_BTN_SVCINSTALL = 1042,
            IDC_BTN_SVCSTART = 1043,
            IDC_BTN_SVCSTOP = 1044,
            IDC_BTN_SVCREMOVE = 1045,
            IDC_BTN_OPENLOG = 1046, // 打开服务日志文件

            IDC_WD_BASE = 1100, // 星期复选框基址（+0..6）
        };

        // 自定义消息：工作线程 → UI
        static constexpr UINT WM_APP_PROGRESS = WM_APP + 1;
        static constexpr UINT WM_APP_DONE = WM_APP + 2;
        static constexpr UINT WM_APP_LOG = WM_APP + 3;
        static constexpr UINT WM_APP_LOCK = WM_APP + 4;
    };

} // namespace dirmv::gui
