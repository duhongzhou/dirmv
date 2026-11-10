# GUI 冒烟测试：启动真实窗口，用 Win32 API 检查界面结构与导航切换。
# 用法：powershell -ExecutionPolicy Bypass -File test\guitest.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\Release\dirMover.exe'

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
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }

    public static IntPtr FindByPid(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h,l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
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

$procId = 0
$passed = 0
$failed = 0
function Check([string]$name, [bool]$ok) {
    if ($ok) { $script:passed++; Write-Host "  [PASS] $name" }
    else { $script:failed++; Write-Host "  [FAIL] $name" -ForegroundColor Red }
}

Write-Host "=== GUI 冒烟测试 ==="
$r = ([wmiclass]'Win32_Process').Create($exe)
if ($r.ReturnValue -ne 0) { Write-Host "启动失败: $($r.ReturnValue)"; exit 1 }
$procId = [int]$r.ProcessId
try {
    $main = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $main -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $main = [G]::FindByPid($procId)
    }
    Check "主窗口已创建" ($main -ne [IntPtr]::Zero)
    if ($main -eq [IntPtr]::Zero) { throw "进程 $procId 未出现可见窗口" }
    Check "主窗口标题含 dirMover" ([G]::Text($main) -like '*dirMover*')

    $kids = [G]::Children($main)
    $navs = @($kids | Where-Object { [G]::Class($_) -eq 'DirMvButton' -and [G]::GetDlgCtrlID($_) -in 2001,2002,2003,2004 })
    Check "左侧四个导航按钮存在" ($navs.Count -eq 4)

    $pages = @($kids | Where-Object { [G]::Class($_) -eq 'dirMoverPage' })
    Check "四个页面容器存在" ($pages.Count -eq 4)

    $lv = $kids | Where-Object { [G]::Class($_) -eq 'SysListView32' } | Select-Object -First 1
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

    # 切到任务页（导航 2002）
    $navTask = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2002 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2002, [IntPtr]$navTask) | Out-Null
    Start-Sleep -Milliseconds 300
    $vis = @($pages | ForEach-Object { [G]::IsWindowVisible($_) })
    Check "任务导航切换到第 2 页" ($vis[1] -and -not $vis[0])

    # 切到计划页（导航 2003）
    $navPlan = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2003 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2003, [IntPtr]$navPlan) | Out-Null
    Start-Sleep -Milliseconds 300
    $vis2 = @($pages | ForEach-Object { [G]::IsWindowVisible($_) })
    Check "计划导航切换到第 3 页" ($vis2[2] -and -not $vis2[1])

    $svc = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1041 } | Select-Object -First 1
    Check "设置页状态文字非空" ($null -ne $svc -and [G]::Text($svc).Length -gt 0)

    $svcStop = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1044 } | Select-Object -First 1
    Check "设置页停止按钮存在(ID 1044)" ($null -ne $svcStop)

    $saveBtn = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1032 } | Select-Object -First 1
    Check "设置页保存按钮存在(ID 1032)" ($null -ne $saveBtn)

    # 切到设置页（导航 2004）
    $navSet = $navs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2004 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2004, [IntPtr]$navSet) | Out-Null
    Start-Sleep -Milliseconds 300
    $vis3 = @($pages | ForEach-Object { [G]::IsWindowVisible($_) })
    Check "设置导航切换到第 4 页" ($vis3[3] -and -not $vis3[2])

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
