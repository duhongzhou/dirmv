# GUI 冒烟测试：启动真实窗口，用 Win32 API 检查界面结构与导航切换。
# 覆盖 1.4.7 的五项左侧导航（规则/任务/大小/计划/设置）与五个页面，共 21 项检查。
# 用法：powershell -ExecutionPolicy Bypass -File test\guitest.ps1 [-Config Release]
param(
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\$Config\dirMover.exe"

if (-not (Test-Path $exe)) { Write-Host "缺少 $exe，请先构建" -ForegroundColor Red; exit 1 }

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class G {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }

    // 优先按窗口类名找主窗口；找不到时回退到该进程第一个可见顶层窗口。
    public static IntPtr FindMain(uint pid) {
        IntPtr any = IntPtr.Zero, main = IntPtr.Zero;
        EnumWindows((h,l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p != pid || !IsWindowVisible(h)) return true;
            if (any == IntPtr.Zero) any = h;
            if (Class(h) == "dirMoverMainWnd") { main = h; return false; }
            return true;
        }, IntPtr.Zero);
        return main != IntPtr.Zero ? main : any;
    }
    public static List<IntPtr> Children(IntPtr parent) {
        var list = new List<IntPtr>();
        EnumChildWindows(parent, (h,l) => { list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Class(IntPtr h) {
        var sb = new StringBuilder(256); GetClassNameW(h, sb, 256); return sb.ToString();
    }
    public static string Text(IntPtr h) {
        var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512); return sb.ToString();
    }
}
'@

function ColumnCount {
    $n = 0
    for ($i = 0; $i -lt 32; $i++) {
        $w = [G]::SendMessageW($script:lv, 0x1000 + 29, [IntPtr]$i, [IntPtr]::Zero)
        if ([int]$w -le 0) { break }
        $n++
    }
    return $n
}

# 把窗口提到最前（TOPMOST -> NOTOPMOST），保证截图/点击时不被其他窗口遮挡。
function Bring-ToFront([IntPtr]$h) {
    [G]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0043) | Out-Null
    [G]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x0043) | Out-Null
}

# 在某个页面容器下按控件 ID 找第一个子控件。
function Find-Ctrl([IntPtr]$parent, [int]$id) {
    return [G]::Children($parent) | Where-Object { [G]::GetDlgCtrlID($_) -eq $id } | Select-Object -First 1
}

# 按「页面专属控件」识别页面，避免依赖 EnumChildWindows 的顺序。
function Find-PageByChildId([int]$id) {
    foreach ($p in $script:pages) {
        if (Find-Ctrl $p $id) { return $p }
    }
    return $null
}

$procId = 0
$passed = 0
$failed = 0
function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:passed++; Write-Host "  [PASS] $name" }
    else { $script:failed++; Write-Host "  [FAIL] $name" -ForegroundColor Red }
}

Write-Host "=== GUI 冒烟测试 ==="
Write-Host "目标：$exe（$((Get-Item $exe).VersionInfo.FileVersion)）"
$r = ([wmiclass]'Win32_Process').Create($exe)
if ($r.ReturnValue -ne 0) { Write-Host "启动失败: $($r.ReturnValue)"; exit 1 }
$procId = [int]$r.ProcessId
try {
    $main = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $main -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $main = [G]::FindMain($procId)
    }
    Check "主窗口已创建" ($main -ne [IntPtr]::Zero)
    if ($main -eq [IntPtr]::Zero) { throw "进程 $procId 未出现可见窗口" }
    Bring-ToFront $main
    Check "主窗口标题含 dirMover" ([G]::Text($main) -like '*dirMover*')

    $kids = [G]::Children($main)
    $navs = @($kids | Where-Object { [G]::Class($_) -eq 'DirMvButton' -and [G]::GetDlgCtrlID($_) -in 2001,2002,2003,2004,2005 })
    Check "左侧五个导航按钮存在(2001-2005)" ($navs.Count -eq 5)

    $pages = @($kids | Where-Object { [G]::Class($_) -eq 'dirMoverPage' })
    $script:pages = $pages
    Check "五个页面容器存在" ($pages.Count -eq 5)

    # 按页面专属控件定位各页：规则=1001 任务=1011 大小=1050 计划=1020 设置=1041
    $pageRules = Find-PageByChildId 1001
    $pageTask = Find-PageByChildId 1011
    $pageSize = Find-PageByChildId 1050
    $pagePlan = Find-PageByChildId 1020
    $pageSettings = Find-PageByChildId 1041

    $lv = $kids | Where-Object { [G]::Class($_) -eq 'SysListView32' -and [G]::GetDlgCtrlID($_) -eq 1001 } | Select-Object -First 1
    $script:lv = $lv
    $cols = 0
    if ($lv) { $cols = ColumnCount }
    Check "规则列表为 6 列" ($cols -eq 6)

    $log = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1011 } | Select-Object -First 1
    Check "日志输出框存在(ID 1011)" ($null -ne $log)

    $migrate = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1007 } | Select-Object -First 1
    Check "任务页开始迁移按钮存在(ID 1007)" ($null -ne $migrate)

    $adopt = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1010 } | Select-Object -First 1
    Check "任务页扫描接管按钮存在(ID 1010)" ($null -ne $adopt)

    # 切到任务页（导航 2002，对应第 2 页）
    $navTask = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2002 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2002, [IntPtr]$navTask) | Out-Null
    Start-Sleep -Milliseconds 300
    Check "任务导航切换到第 2 页" ([G]::IsWindowVisible($pageTask) -and -not [G]::IsWindowVisible($pageRules))

    # 切到大小页（导航 2003，对应第 3 页）
    $navSize = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2003 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2003, [IntPtr]$navSize) | Out-Null
    Start-Sleep -Milliseconds 400
    Check "大小导航切换到第 3 页" ([G]::IsWindowVisible($pageSize) -and -not [G]::IsWindowVisible($pageTask))

    $sizeRule = Find-Ctrl $pageSize 1050
    $sizeRefresh = Find-Ctrl $pageSize 1051
    $sizeWatchLv = Find-Ctrl $pageSize 1052
    $sizeTargetLv = Find-Ctrl $pageSize 1053
    Check "大小页规则下拉存在且可见(ID 1050)" ($null -ne $sizeRule -and [G]::IsWindowVisible($sizeRule))
    Check "大小页刷新按钮存在且可见(ID 1051)" ($null -ne $sizeRefresh -and [G]::IsWindowVisible($sizeRefresh))
    Check "大小页监视目录列表存在且可见(ID 1052)" ($null -ne $sizeWatchLv -and [G]::IsWindowVisible($sizeWatchLv))
    Check "大小页目标目录列表存在且可见(ID 1053)" ($null -ne $sizeTargetLv -and [G]::IsWindowVisible($sizeTargetLv))

    # 点击刷新：只要求可点且不崩溃，不强制等待扫描完成（大目录可能耗时）。
    Bring-ToFront $main
    [G]::SendMessageW($main, 0x0111, [IntPtr]1051, [IntPtr]$sizeRefresh) | Out-Null
    Start-Sleep -Milliseconds 400
    $alive = $null -ne (Get-Process -Id $procId -ErrorAction SilentlyContinue)
    Check "大小页刷新按钮可点击（点击后进程存活）" ($alive -and [G]::IsWindowVisible($main))

    # 切到计划页（导航 2004，对应第 4 页）
    $navPlan = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2004 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2004, [IntPtr]$navPlan) | Out-Null
    Start-Sleep -Milliseconds 300
    Check "计划导航切换到第 4 页" ([G]::IsWindowVisible($pagePlan) -and -not [G]::IsWindowVisible($pageSize))

    # 切到设置页（导航 2005，对应第 5 页）
    $navSet = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2005 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2005, [IntPtr]$navSet) | Out-Null
    Start-Sleep -Milliseconds 300
    Check "设置导航切换到第 5 页" ([G]::IsWindowVisible($pageSettings) -and -not [G]::IsWindowVisible($pagePlan))

    $svc = Find-Ctrl $pageSettings 1041
    Check "设置页状态文字非空(ID 1041)" ($null -ne $svc -and [G]::Text($svc).Length -gt 0)

    $svcStop = Find-Ctrl $pageSettings 1044
    Check "设置页停止按钮存在(ID 1044)" ($null -ne $svcStop)

    $saveBtn = Find-Ctrl $pageSettings 1032
    Check "设置页保存按钮存在(ID 1032)" ($null -ne $saveBtn)

    [G]::PostMessageW($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 500
    Check "关闭后隐藏到托盘" (-not [G]::IsWindowVisible($main))
}
finally {
    if ($procId -gt 0) { Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 200
}

Write-Host "=== 结果：通过 $passed，失败 $failed ==="
if ($failed -gt 0) { exit 1 }
exit 0
