# GUI 冒烟测试：启动真实窗口，用 Win32 API 检查界面结构与页签切换。
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
    [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMsg3(IntPtr h, uint m, IntPtr w, IntPtr l);
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
    // comctl32: LVM_GETCOLUMNWIDTH 无直接"列数"消息，用 Header 子窗口项数
    public const uint LVM_GETHEADER = 0x1000 + 31;
    public const uint HDM_GETITEMCOUNT = 0x1200 + 0;
}
'@

function FrameCounts([int]$mask) {
    # 取所有列宽：逐列发送 LVM_GETCOLUMNWIDTH，返回 -1 表示结束
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
# 分离启动，避免继承管道
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
    $tabs = @($kids | Where-Object { [G]::Class($_) -eq 'DirMvButton' -and [G]::GetDlgCtrlID($_) -in 2001,2002,2003 })
    Check "顶部三个页签按钮存在" ($tabs.Count -eq 3)

    $pages = @($kids | Where-Object { [G]::Class($_) -eq 'dirMoverPage' })
    Check "三个页面容器存在" ($pages.Count -eq 3)

    $lv = $kids | Where-Object { [G]::Class($_) -eq 'SysListView32' } | Select-Object -First 1
    $script:lv = $lv
    $cols = 0
    if ($lv) { $cols = FrameCounts 0 }
    Check "规则列表为 6 列" ($cols -eq 6)

    $log = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1011 } | Select-Object -First 1
    Check "日志输出框存在(ID 1011)" ($null -ne $log)

    # 切到计划页
    $tabPlan = $tabs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2002 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2002, [IntPtr]$tabPlan) | Out-Null  # WM_COMMAND BN_CLICKED -> 父窗口
    Start-Sleep -Milliseconds 300

    # 检查可见性：页 1 可见、页 0 隐藏
    $vis = @($pages | ForEach-Object { [G]::IsWindowVisible($_) })
    Check "计划页签切换到第 2 页" ($vis[1] -and -not $vis[0])

    $tabAbout = $tabs | Where-Object { [G]::GetDlgCtrlID($_) -eq 2003 }
    [G]::SendMessageW($main, 0x0111, [IntPtr]2003, [IntPtr]$tabAbout) | Out-Null
    Start-Sleep -Milliseconds 300
    $vis2 = @($pages | ForEach-Object { [G]::IsWindowVisible($_) })
    Check "关于页签切换到第 3 页" ($vis2[2] -and -not $vis2[1])

    $svc = $kids | Where-Object { [G]::GetDlgCtrlID($_) -eq 1041 } | Select-Object -First 1
    Check "关于页服务状态文字非空" ($null -ne $svc -and [G]::Text($svc).Length -gt 0)

    [G]::PostMessageW($main, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
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
