# ---------------------------------------------------------------------------
# install_local.ps1 - 以管理员身份：卸载旧版 → 安装新版 → 配置白名单 → 启动服务
# 直接运行（需管理员）：pwsh -File installer\install_local.ps1
# 结果写入 build\install_elevated.log
# ---------------------------------------------------------------------------
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
$log = Join-Path $root "build\install_elevated.log"
if (Test-Path $log) { Remove-Item $log -Force }
function Log($m) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
    Write-Host $line
}

$ident = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = ([System.Security.Principal.WindowsPrincipal]$ident).IsInRole(
    [System.Security.Principal.WindowsBuiltInRole]::Administrator)
Log "提权进程: $($ident.Name) admin=$isAdmin"; Log ((whoami /groups | Select-String "Mandatory Label").Line)
if (-not $isAdmin) { Log "FAIL: 未取得管理员权限"; exit 1 }

# 1) 停服务
Get-Service dirMoverSvc -ErrorAction SilentlyContinue | ForEach-Object {
    Log "停止服务 dirMoverSvc"
    Stop-Service dirMoverSvc -Force -ErrorAction SilentlyContinue
}
# 顺带清理自动重试计划任务，避免安装过程中被拉起
schtasks /Delete /TN dirMover_RetryOnce /F 2>&1 | Out-Null

# 2) 卸载已安装的旧版（按卸载注册表枚举，兼容产品码变化）
$uninstallRoots = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"
)
$codes = @()
foreach ($r in $uninstallRoots) {
    Get-ChildItem $r -ErrorAction SilentlyContinue | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        if ($p.DisplayName -like "*dirMover*") { $codes += $_.PSChildName }
    }
}
if (-not $codes) {
    $codes = @("{B45CE44F-1EA4-4289-AB57-1DFBA80C39CA}")
}
foreach ($code in $codes) {
    Log "卸载 MSI $code"
    $p = Start-Process msiexec.exe -ArgumentList "/x", $code, "/qn", "/norestart",
        "/l*v", "$root\build\msi_uninstall.log" -Wait -PassThru
    Log "卸载退出码 $($p.ExitCode)"
}

# 3) 安装新版
$msi = Get-ChildItem (Join-Path $root "installer\dist\dirMover-*.msi") | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object { $_.FullName }
if (-not (Test-Path $msi)) { Log "FAIL: 找不到安装包 $msi"; exit 1 }
Log "安装 $msi"
$p = Start-Process msiexec.exe -ArgumentList "/i", "`"$msi`"", "/qn", "/norestart",
    "/l*v", "$root\build\msi_install.log" -Wait -PassThru
Log "安装退出码 $($p.ExitCode)"
if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { Log "FAIL: 安装失败"; exit 1 }

# 4) 配置：把正在被应用使用的 .dsh 加入白名单（防止再次迁移 DSH 数据）
$cli = "C:\Program Files\dirMover\dirMover_cli.exe"
if (Test-Path $cli) {
    Log "配置白名单: .dsh"
    $out = & $cli exclude --index 0 --name ".dsh" 2>&1 | Out-String
    Log ("exclude 输出: " + ($out.Trim() -replace "`r`n", " | "))
} else {
    Log "WARN: 找不到 $cli"
}

# 5) 安装并启动服务（开机自启）
if (Test-Path $cli) {
    $out = & $cli install 2>&1 | Out-String
    Log ("install 输出: " + ($out.Trim() -replace "`r`n", " | "))
    $out = & $cli start 2>&1 | Out-String
    Log ("start 输出: " + ($out.Trim() -replace "`r`n", " | "))
}

# 6) 校验
$exe = "C:\Program Files\dirMover\dirMover.exe"
if (Test-Path $exe) {
    $v = (Get-Item $exe).VersionInfo.FileVersion
    Log "已安装: $exe 版本 $v"
} else {
    Log "FAIL: 目标文件不存在"
}
Get-Service dirMoverSvc -ErrorAction SilentlyContinue | ForEach-Object {
    Log "服务状态: $($_.Status) 启动类型: $($_.StartType)"
}
$qc = (& sc.exe qc dirMoverSvc 2>&1 | Out-String)
$startLine = (($qc -split "`r?`n") | Where-Object { $_ -match 'START_TYPE' } | Select-Object -First 1)
Log ("服务启动配置: " + $startLine.Trim())
if ($startLine -match 'DELAYED') { Log "WARN: 仍为延迟启动，开机后约 2 分钟才启动" }
$qf = (& sc.exe qfailure dirMoverSvc 2>&1 | Out-String)
$actLines = (($qf -split "`r?`n") | Where-Object { $_ -match 'RESTART|RESET_PERIOD' } | Select-Object -First 3)
Log ("失败恢复配置: " + (($actLines | ForEach-Object { $_.Trim() }) -join '; '))
Log "DONE"
exit 0
