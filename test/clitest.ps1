# ---------------------------------------------------------------------------
# clitest.ps1 - 用「测试规则文件」通过 CLI 驱动，逐项验证 dirMover 功能
#
#   安全：所有操作限定在 test\sandbox 之下（TestCommon.psm1 的 Assert-InSandbox）
#   用法：powershell -ExecutionPolicy Bypass -File test\clitest.ps1 [-Config Release]
# ---------------------------------------------------------------------------
param(
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "TestCommon.psm1") -Force

$root = Get-ProjectRoot $PSScriptRoot
$exe = Join-Path $root "build\$Config\dirMover_cli.exe"
$sandk = Join-Path $root "test\sandbox"

$pass = 0; $fail = 0
function Ok($m) { Write-Host "  [PASS] $m" -ForegroundColor Green; $script:pass++ }
function Bad($m) { Write-Host "  [FAIL] $m" -ForegroundColor Red; $script:fail++ }
function Info($m) { Write-Host "  $m" -ForegroundColor Gray }
function Head($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

if (-not (Test-Path $exe)) { Write-Host "找不到 $exe，请先构建。" -ForegroundColor Red; exit 1 }

New-Item -ItemType Directory -Force -Path $sandk | Out-Null

# ===========================================================================
Head "功能1: 测试规则文件 + scan"
$s1 = Join-Path $sandk "c1\home"; $d1 = Join-Path $sandk "c1\store"
New-TestTree -Base $s1 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d1 | Out-Null
$ruleFile = Join-Path $sandk "c1.ini"
New-TestRuleFile -Path $ruleFile -Sandbox $sandk -Rules @(@{w = $s1; t = $d1; ex = "Desktop|Documents" }) | Out-Null
Info "测试规则文件: $ruleFile"
$o = Invoke-Cli -Exe $exe -ConfigPath $ruleFile -Sandbox $sandk -Args @("scan")
if ($o -match "A" -and $o -match "B" -and $o -notmatch "Desktop") { Ok "scan 列出可迁移项且排除白名单" } else { Bad "scan 结果异常" }

# ===========================================================================
Head "功能2: 迁移 + 链接 + 数据"
$o = Invoke-Cli -Exe $exe -ConfigPath $ruleFile -Sandbox $sandk -Args @("migrate")
$itA = Get-Item "$s1\A" -Force
if ($itA.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "A 变为链接" } else { Bad "A 未迁移" }
if ((Get-Content "$s1\A\sub\a.txt") -eq "helloA") { Ok "经链接读取数据正确" } else { Bad "数据读取错误" }
if (Test-Path "$d1\A\sub\a.txt") { Ok "目标目录含数据" } else { Bad "目标缺数据" }

# ===========================================================================
Head "功能3: 恢复"
$o = Invoke-Cli -Exe $exe -ConfigPath $ruleFile -Sandbox $sandk -Args @("restore")
$itA = Get-Item "$s1\A" -Force
if (-not ($itA.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "A 已还原为实体目录" } else { Bad "A 仍为链接" }
if ((Get-Content "$s1\A\sub\a.txt") -eq "helloA") { Ok "还原后数据完整" } else { Bad "还原后数据丢失" }

# ===========================================================================
Head "功能4: 更换目标目录 (change-target)"
$s4 = Join-Path $sandk "c4\home"; $d4a = Join-Path $sandk "c4\storeA"; $d4b = Join-Path $sandk "c4\storeB"
New-TestTree -Base $s4 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d4a, $d4b | Out-Null
$rf4 = Join-Path $sandk "c4.ini"
New-TestRuleFile -Path $rf4 -Sandbox $sandk -Rules @(@{w = $s4; t = $d4a }) | Out-Null
Invoke-Cli -Exe $exe -ConfigPath $rf4 -Sandbox $sandk -Args @("migrate") | Out-Null
$o = Invoke-Cli -Exe $exe -ConfigPath $rf4 -Sandbox $sandk -Args @("change-target", "--index", "0", "--newtarget", $d4b)
$tgt = (Get-Item "$s4\A" -Force).Target
if ($tgt -and ($tgt -join "") -like "*storeB*") { Ok "链接已重指向新目标" } else { Bad "链接未重指向: $tgt" }
if ((Test-Path "$d4b\A\sub\a.txt") -and -not (Test-Path "$d4a\A")) { Ok "数据已搬迁到新目标" } else { Bad "数据搬迁异常" }

# ===========================================================================
Head "功能5: 白名单还原 (restore-excluded)"
$s5 = Join-Path $sandk "c5\home"; $d5 = Join-Path $sandk "c5\store"
New-TestTree -Base $s5 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d5 | Out-Null
$rf5 = Join-Path $sandk "c5.ini"
New-TestRuleFile -Path $rf5 -Sandbox $sandk -Rules @(@{w = $s5; t = $d5; ex = "" }) | Out-Null
Invoke-Cli -Exe $exe -ConfigPath $rf5 -Sandbox $sandk -Args @("migrate") | Out-Null
$o = Invoke-Cli -Exe $exe -ConfigPath $rf5 -Sandbox $sandk -Args @("restore-excluded", "--index", "0", "--name", "A")
$itA = Get-Item "$s5\A" -Force
if (-not ($itA.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "A 已还原(移出迁移)" } else { Bad "A 仍为链接" }

# ===========================================================================
Head "功能6: 扫描已有迁移 (adopt)"
$s6 = Join-Path $sandk "c6\home"; $d6 = Join-Path $sandk "c6\store"
New-TestTree -Base $s6 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d6 | Out-Null
$rf6 = Join-Path $sandk "c6.ini"
New-TestRuleFile -Path $rf6 -Sandbox $sandk -Rules @(@{w = $s6; t = $d6 }) | Out-Null
Invoke-Cli -Exe $exe -ConfigPath $rf6 -Sandbox $sandk -Args @("migrate") | Out-Null
Remove-Item "$s6\.dirmv" -Recurse -Force -EA SilentlyContinue   # 模拟无记录
$o = Invoke-Cli -Exe $exe -ConfigPath $rf6 -Sandbox $sandk -Args @("adopt", "--index", "0")
if ($o -match "新增纳入 2") { Ok "adopt 识别出 2 个已有迁移" } else { Bad "adopt 结果异常: $o" }

# ===========================================================================
Head "功能7: 新机器重建链接 (rebuild)"
$s7 = Join-Path $sandk "c7\home"; $d7 = Join-Path $sandk "c7\store"; $n7 = Join-Path $sandk "c7\newhome"
New-TestTree -Base $s7 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d7 | Out-Null
$rf7 = Join-Path $sandk "c7.ini"
New-TestRuleFile -Path $rf7 -Sandbox $sandk -Rules @(@{w = $s7; t = $d7 }) | Out-Null
Invoke-Cli -Exe $exe -ConfigPath $rf7 -Sandbox $sandk -Args @("migrate") | Out-Null
$o = Invoke-Cli -Exe $exe -ConfigPath $rf7 -Sandbox $sandk -Args @("rebuild", "--index", "0", "--watch", $n7)
$itA = Get-Item "$n7\A" -Force -EA SilentlyContinue
if ($itA -and ($itA.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "新监视目录已重建链接" } else { Bad "重建失败" }

# ===========================================================================
Head "功能8: 删除规则(先还原) (delete-rule)"
$s8 = Join-Path $sandk "c8\home"; $d8 = Join-Path $sandk "c8\store"
New-TestTree -Base $s8 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d8 | Out-Null
$rf8 = Join-Path $sandk "c8.ini"
New-TestRuleFile -Path $rf8 -Sandbox $sandk -Rules @(@{w = $s8; t = $d8 }) | Out-Null
Invoke-Cli -Exe $exe -ConfigPath $rf8 -Sandbox $sandk -Args @("migrate") | Out-Null
$o = Invoke-Cli -Exe $exe -ConfigPath $rf8 -Sandbox $sandk -Args @("delete-rule", "--index", "0")
$itA = Get-Item "$s8\A" -Force -EA SilentlyContinue
if ($itA -and -not ($itA.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "删除规则前已还原数据" } else { Bad "删除前未还原" }
$cfgText = Get-Content $rf8 -Raw -Encoding UTF8
if ($cfgText -notmatch [regex]::Escape($s8)) { Ok "规则已从配置删除" } else { Bad "规则仍在配置中" }

# ===========================================================================
Head "功能9: 导入规则 (import)"
$s9 = Join-Path $sandk "c9\home"; $d9 = Join-Path $sandk "c9\store"
New-TestTree -Base $s9 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d9 | Out-Null
$rf9 = Join-Path $sandk "c9.ini"
New-TestRuleFile -Path $rf9 -Sandbox $sandk -Rules @(@{w = $s9; t = $d9 }) | Out-Null
Invoke-Cli -Exe $exe -ConfigPath $rf9 -Sandbox $sandk -Args @("scan") | Out-Null  # 写配置到目录
$rf9b = Join-Path $sandk "c9b.ini"
New-TestRuleFile -Path $rf9b -Sandbox $sandk -Rules @(@{w = (Join-Path $sandk "c9\other"); t = (Join-Path $sandk "c9\otherDst") }) | Out-Null
$o = Invoke-Cli -Exe $exe -ConfigPath $rf9b -Sandbox $sandk -Args @("import", "--dir", $s9)
$cfg2 = Get-Content $rf9b -Raw -Encoding UTF8
if ($cfg2 -match [regex]::Escape($s9) -and $cfg2 -match [regex]::Escape($d9)) { Ok "导入规则成功" } else { Bad "导入失败" }

# ===========================================================================
Head "功能10: 占用检测 (锁定文件 → 跳过该子任务)"
$s10 = Join-Path $sandk "c10\home"; $d10 = Join-Path $sandk "c10\store"
New-TestTree -Base $s10 -Sandbox $sandk
New-Item -ItemType Directory -Force -Path $d10 | Out-Null
$rf10 = Join-Path $sandk "c10.ini"
New-TestRuleFile -Path $rf10 -Sandbox $sandk -Rules @(@{w = $s10; t = $d10 }) | Out-Null
$fs = [System.IO.File]::Open("$s10\A\sub\a.txt", 'Open', 'ReadWrite', 'None')
try {
    $o = Invoke-Cli -Exe $exe -ConfigPath $rf10 -Sandbox $sandk -Args @("migrate")
    if ($o -match "占用") { Ok "报告了文件占用" } else { Bad "未报告占用" }
    $itA = Get-Item "$s10\A" -Force
    if (-not ($itA.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "被占用子任务保持实体(未半迁移)" } else { Bad "被占用子任务被迁移" }
    $itB = Get-Item "$s10\B" -Force
    if ($itB.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "其他子任务不受影响" } else { Bad "占用影响了其他子任务" }
}
finally { $fs.Close() }

# ===========================================================================
Head "功能11: 配置防篡改 (HMAC 签名)"
$cfgW = "$s10\.dirmv\dirmv.ini"
if (Test-Path $cfgW) {
    $txt = Get-Content $cfgW -Raw -Encoding UTF8
    if ($txt -match "#SIG:[0-9a-f]{64}") { Ok "配置含签名" } else { Bad "配置缺签名" }
    Write-Utf8NoBom -Path $cfgW -Text ($txt -replace "Exclude=", "Exclude=Hacked|")
    Remove-Item (Join-Path $root "build\$Config\dirmv_cli.log") -Force -EA SilentlyContinue
    Invoke-Cli -Exe $exe -ConfigPath $cfgW -Sandbox $sandk -Args @("scan") | Out-Null
    $log = ""
    $logPath = Join-Path $root "build\$Config\dirmv_cli.log"
    if (Test-Path $logPath) { $log = Get-Content $logPath -Raw -Encoding UTF8 }
    if ($log -match "签名不匹配") { Ok "篡改被检出" } else { Bad "篡改未检出" }
}
else { Bad "监视目录未写配置" }

# ===========================================================================
Head "结果"
Write-Host "  通过: $pass   失败: $fail" -ForegroundColor $(if ($fail -eq 0) { "Green" } else { "Red" })
if ($fail -eq 0) { exit 0 } else { exit 1 }
