# ---------------------------------------------------------------------------
# safety_test.ps1 - 迁移安全性测试
#   验证「任何失败路径都不会删除唯一副本」这一核心不变量：
#   1. 正常迁移：源变为链接、目标数据完整、无临时残留
#   2. 源目录被占用：不迁移、源结构完整、目标不留下数据
#   3. 目录联接(Junction)：创建的是真正的联接，数据可读
#   4. 白名单命令：exclude / unexclude 生效
#   全部在 test\sandbox 下运行，不碰真实目录。
# ---------------------------------------------------------------------------
param([string]$Config = "Release")

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot "TestCommon.psm1") -Force

$exe = Join-Path $root "build\$Config\dirMover_cli.exe"
$sandk = Join-Path $PSScriptRoot "sandbox"
if (-not (Test-Path $exe)) { Write-Host "找不到 $exe，请先构建" -ForegroundColor Red; exit 1 }

$script:pass = 0
$script:fail = 0
function Ok($m) { $script:pass++; Write-Host "  [PASS] $m" -ForegroundColor Green }
function Bad($m) { $script:fail++; Write-Host "  [FAIL] $m" -ForegroundColor Red }
function Head($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

function Write-Config {
    param([string]$Path, [array]$Rules, [int]$LinkType = 0)
    Assert-InSandbox -Path $Path -Sandbox $sandk | Out-Null
    $sb = New-Object System.Text.StringBuilder
    $i = 1
    foreach ($r in $Rules) {
        [void]$sb.AppendLine("[Rule$i]")
        [void]$sb.AppendLine("WatchDir=$($r.w)")
        [void]$sb.AppendLine("TargetDir=$($r.t)")
        [void]$sb.AppendLine("Exclude=$($r.ex)")
        [void]$sb.AppendLine("Enabled=1")
        [void]$sb.AppendLine("")
        $i++
    }
    [void]$sb.AppendLine("[Migrate]")
    [void]$sb.AppendLine("LinkType=$LinkType")
    [void]$sb.AppendLine("MoveMode=1")
    [void]$sb.AppendLine("SkipExisting=1")
    [void]$sb.AppendLine("WorkerThreads=1")
    [void]$sb.AppendLine("EnableWatcher=0")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("[Lock]")
    [void]$sb.AppendLine("Detect=1")
    [void]$sb.AppendLine("RetryDelayMinutes=5")
    Write-Utf8NoBom -Path $Path -Text $sb.ToString()
}

function Invoke-CliText {
    param([string]$ConfigPath, [string[]]$CliArgs)
    return (& $exe @CliArgs --config $ConfigPath 2>&1 | Out-String)
}

New-Item -ItemType Directory -Force -Path $sandk | Out-Null

# 统一使用目录联接(Junction)：不依赖管理员/开发者模式，测试结果稳定。
# （符号链接的创建权限取决于运行环境，由 test 套件中的正常迁移路径覆盖。）
$linkType = 1

# ---------------------------------------------------------------------------
Head "安全测试1: 正常迁移（无临时残留、数据完整）"
$src = "$sandk\s1\src"; $dst = "$sandk\s1\dst"
New-TestTree -Base $src -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\s1.ini"
Write-Config -Path $cfg -Rules @(@{ w = $src; t = $dst; ex = "Desktop" }) -LinkType $linkType
$o = Invoke-CliText -ConfigPath $cfg -CliArgs @("migrate")
if ($o -match "失败 0") { Ok "迁移报告无失败" } else { Bad "迁移有失败: $($o.Trim())" }
$ait = Get-Item "$src\A" -Force
if ($ait.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "源位置已是链接" } else { Bad "源位置不是链接" }
if ((Get-Content "$src\A\sub\a.txt") -eq "helloA") { Ok "经链接读取数据正确" } else { Bad "经链接读取数据错误" }
$leftovers = Get-ChildItem $src -Force | Where-Object { $_.Name -like "*.dirmv-moving*" -or $_.Name -like "*.dirmv-old*" }
if (-not $leftovers) { Ok "无临时目录残留" } else { Bad "存在临时残留: $($leftovers.Name -join ', ')" }

# ---------------------------------------------------------------------------
Head "安全测试2: 源目录被占用（不迁移、源完整、不产生目标数据）"
$src = "$sandk\s2\src"; $dst = "$sandk\s2\dst"
New-TestTree -Base $src -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\s2.ini"
Write-Config -Path $cfg -Rules @(@{ w = $src; t = $dst; ex = "Desktop" }) -LinkType $linkType
$lockPath = "$src\A\sub\a.txt"
$fs = [System.IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'None')
try {
    $o = Invoke-CliText -ConfigPath $cfg -CliArgs @("migrate")
    if ($o -match "占用") { Ok "报告了占用" } else { Bad "未报告占用: $($o.Trim())" }
    $ait = Get-Item "$src\A" -Force
    if (-not ($ait.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "被占用目录保持实体" } else { Bad "被占用目录被迁移（不应发生）" }
    if ((Test-Path "$src\A\sub\a.txt") -and (Test-Path "$src\A\root.txt")) { Ok "源目录结构完整（未丢数据）" } else { Bad "源目录结构被破坏" }
    if (Test-Path "$dst\A") { Bad "被占用时目标仍产生了数据" } else { Ok "未在目标留下半成品" }
    # B 不受影响，应已迁移
    $bit = Get-Item "$src\B" -Force
    if ($bit.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "其他子任务不受影响" } else { Bad "占用影响了其他子任务" }
}
finally { $fs.Close() }

# ---------------------------------------------------------------------------
Head "安全测试3: 目录联接(Junction) 真联接"
$src = "$sandk\s3\src"; $dst = "$sandk\s3\dst"
New-TestTree -Base $src -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\s3.ini"
Write-Config -Path $cfg -Rules @(@{ w = $src; t = $dst; ex = "Desktop" }) -LinkType 1
$o = Invoke-CliText -ConfigPath $cfg -CliArgs @("migrate")
if ($o -match "失败 0") { Ok "Junction 迁移无失败" } else { Bad "Junction 迁移失败: $($o.Trim())" }
$ait = Get-Item "$src\A" -Force
if ($ait.LinkType -eq "Junction") { Ok "创建的是真 Directory Junction" } else { Bad "链接类型不是 Junction: $($ait.LinkType)" }
if ((Get-Content "$src\A\sub\a.txt") -eq "helloA") { Ok "经 Junction 读取数据正确" } else { Bad "经 Junction 读取数据错误" }

# ---------------------------------------------------------------------------
Head "安全测试4: 白名单命令 exclude / unexclude"
$src = "$sandk\s4\src"; $dst = "$sandk\s4\dst"
New-TestTree -Base $src -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\s4.ini"
Write-Config -Path $cfg -Rules @(@{ w = $src; t = $dst; ex = "Desktop" }) -LinkType $linkType
$o = Invoke-CliText -ConfigPath $cfg -CliArgs @("exclude", "--index", "0", "--name", "A")
if ($o -match "已加入白名单") { Ok "exclude 命令成功" } else { Bad "exclude 命令失败: $($o.Trim())" }
$scan = Invoke-CliText -ConfigPath $cfg -CliArgs @("scan")
if ($scan -notmatch "`n\s*A\s*`n") { Ok "白名单目录不再出现在待迁移列表" } else { Bad "白名单目录仍待迁移: $($scan.Trim())" }
$cfgText = Get-Content $cfg -Raw -Encoding UTF8
if ($cfgText -match "Exclude=.*\bA\b") { Ok "配置已写入白名单" } else { Bad "配置未写入白名单" }
$o = Invoke-CliText -ConfigPath $cfg -CliArgs @("unexclude", "--index", "0", "--name", "A")
if ($o -match "已从白名单移除") { Ok "unexclude 命令成功" } else { Bad "unexclude 命令失败: $($o.Trim())" }
$scan = Invoke-CliText -ConfigPath $cfg -CliArgs @("scan")
if ($scan -match "A") { Ok "移除后重新出现在待迁移列表" } else { Bad "移除后仍未出现在待迁移列表" }

# ---------------------------------------------------------------------------
Write-Host "`n================= 安全测试汇总 =================" -ForegroundColor Magenta
Write-Host ("  通过 {0} 项，失败 {1} 项" -f $script:pass, $script:fail) -ForegroundColor $(if ($script:fail -eq 0) { "Green" } else { "Red" })
if ($script:fail -eq 0) { exit 0 } else { exit 1 }
