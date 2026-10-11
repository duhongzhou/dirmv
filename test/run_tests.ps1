# ---------------------------------------------------------------------------
# run_tests.ps1 - dirMover 功能测试
#
# 在项目目录下创建测试树（test\sandbox），覆盖：
#   1. 基本迁移 + 链接创建 + 数据完整性
#   2. 白名单（排除）目录不被迁移
#   3. 恢复后数据完整、链接移除
#   4. 多规则同时迁移
#   5. 目标目录重复 / 监视目录重叠 → 配置校验应报错
#   6. 目标已存在同名目录 → 跳过
#   7. 占用检测：锁定文件 → 该子任务被跳过(Locked)，源目录保持完整，其他子任务正常
#   8. 原子性：失败子任务完整回滚（源目录结构不变）
#
# 用法:  powershell -ExecutionPolicy Bypass -File test\run_tests.ps1
# ---------------------------------------------------------------------------
param(
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot          # 项目根
$exe = Join-Path $root "build\$Config\dirMover_cli.exe"
$sandk = Join-Path $root "test\sandbox"

$pass = 0
$fail = 0

function Ok($m) { Write-Host "  [PASS] $m" -ForegroundColor Green; $script:pass++ }
function Bad($m) { Write-Host "  [FAIL] $m" -ForegroundColor Red; $script:fail++ }
function Info($m) { Write-Host "  $m" -ForegroundColor Gray }
function Head($m) { Write-Host "`n=== $m ===" -ForegroundColor Cyan }

if (-not (Test-Path $exe)) { Write-Host "找不到 $exe，请先构建。" -ForegroundColor Red; exit 1 }

# --- 工具函数 -------------------------------------------------------------
function Write-Utf8NoBom($path, $text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# 生成配置文件：$rules = @(@{w=..;t=..}, ...) ；白名单为每条规则独立（$exclude）
function Write-Config($path, $rules, $exclude = "Desktop|Documents") {
    $sb = New-Object System.Text.StringBuilder
    $i = 1
    foreach ($r in $rules) {
        [void]$sb.AppendLine("[Rule$i]")
        [void]$sb.AppendLine("WatchDir=$($r.w)")
        [void]$sb.AppendLine("TargetDir=$($r.t)")
        [void]$sb.AppendLine("Exclude=$exclude")
        [void]$sb.AppendLine("Enabled=1")
        [void]$sb.AppendLine("")
        $i++
    }
    [void]$sb.AppendLine("[Migrate]")
    [void]$sb.AppendLine("LinkType=1")      # Junction（测试无需管理员）
    [void]$sb.AppendLine("MoveMode=1")
    [void]$sb.AppendLine("SkipExisting=1")
    [void]$sb.AppendLine("WorkerThreads=2")
    [void]$sb.AppendLine("EnableWatcher=0")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("[Lock]")
    [void]$sb.AppendLine("Detect=1")
    [void]$sb.AppendLine("RetryDelayMinutes=5")
    Write-Utf8NoBom $path $sb.ToString()
}

function New-Tree($base) {
    # $base\A\sub\a.txt, $base\B\b.bin, $base\Desktop\d.txt
    New-Item -ItemType Directory -Force -Path "$base\A\sub", "$base\B", "$base\Desktop" | Out-Null
    Set-Content -Path "$base\A\sub\a.txt" -Value "helloA" -Encoding ascii
    Set-Content -Path "$base\A\root.txt"  -Value "rootA"  -Encoding ascii
    Set-Content -Path "$base\B\b.bin"     -Value "dataB"  -Encoding ascii
    Set-Content -Path "$base\Desktop\d.txt" -Value "desk" -Encoding ascii
}

function Reset-Sandbox() {
    if (Test-Path $sandk) {
        # 先移除可能的链接，避免递归删除目标数据
        Get-ChildItem -Path $sandk -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } |
        ForEach-Object { cmd /c "rmdir `"$($_.FullName)`"" 2>$null }
        Remove-Item $sandk -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Force -Path $sandk | Out-Null
}

# 运行 CLI 并返回输出
function Invoke-Cli($cfg, $cmd) {
    $out = & $exe $cmd --config $cfg 2>&1 | Out-String
    return $out
}

# ===========================================================================
Head "准备测试环境"
Reset-Sandbox
Write-Host "  沙箱: $sandk"
Ok "沙箱已创建"

# ---------------------------------------------------------------------------
Head "测试1: 基本迁移 + 链接 + 数据完整"
$src = "$sandk\t1\src"; $dst = "$sandk\t1\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t1.ini"
Write-Config $cfg @(@{w = $src; t = $dst })

$o = Invoke-Cli $cfg "scan"
Info ($o.Trim())
if ($o -match "A" -and $o -match "B" -and $o -notmatch "待迁移目录 0") { Ok "scan 列出待迁移目录" } else { Bad "scan 未列出目录" }

$o = Invoke-Cli $cfg "migrate"
Info ($o.Trim() -split "`n" | Select-Object -Last 1)
# 链接是否创建（A、B 应为 Junction）
$aLink = Get-Item "$src\A" -Force
if ($aLink.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "A 已变为链接" } else { Bad "A 不是链接" }
if (Test-Path "$dst\A\sub\a.txt") { Ok "目标含全部文件" } else { Bad "目标缺少文件" }
if ((Get-Content "$src\A\sub\a.txt") -eq "helloA") { Ok "经链接读取内容正确" } else { Bad "链接读取内容错误" }
if (-not (Test-Path "$src\Desktop\.keep") -and (Test-Path "$src\Desktop\d.txt")) { Ok "白名单 Desktop 未被迁移" } else { Bad "白名单目录被迁移了" }
# 源 A 里应已无实体文件（成为链接）
if (-not (Test-Path "$dst\Desktop")) { Ok "白名单未出现在目标" } else { Bad "目标出现白名单目录" }

# ---------------------------------------------------------------------------
Head "测试2: 恢复 + 数据完整"
$o = Invoke-Cli $cfg "restore"
Info ($o.Trim() -split "`n" | Select-Object -Last 1)
$aAfter = Get-Item "$src\A" -Force
if (-not ($aAfter.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "A 链接已移除，恢复为实体目录" } else { Bad "A 仍是链接" }
if ((Get-Content "$src\A\sub\a.txt") -eq "helloA") { Ok "恢复后内容完整" } else { Bad "恢复后内容丢失" }
if (-not (Test-Path "$dst\A")) { Ok "目标数据已清空" } else { Bad "目标数据未清空" }

# ---------------------------------------------------------------------------
Head "测试3: 多规则同时迁移"
$srcA = "$sandk\t3\srcA"; $dstA = "$sandk\t3\dstA"
$srcB = "$sandk\t3\srcB"; $dstB = "$sandk\t3\dstB"
New-Tree $srcA; New-Tree $srcB
New-Item -ItemType Directory -Force -Path $dstA, $dstB | Out-Null
$cfg = "$sandk\t3.ini"
Write-Config $cfg @(@{w = $srcA; t = $dstA }, @{w = $srcB; t = $dstB })
$o = Invoke-Cli $cfg "migrate"
Info ($o.Trim() -split "`n" | Select-Object -Last 1)
$la = Get-Item "$srcA\A" -Force; $lb = Get-Item "$srcB\A" -Force
if (($la.Attributes -band [IO.FileAttributes]::ReparsePoint) -and ($lb.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "两条规则均完成迁移" } else { Bad "多规则迁移失败" }

# ---------------------------------------------------------------------------
Head "测试4: 配置校验 (目标重复 / 监视重叠)"
# 4a 目标重复
$cfg = "$sandk\t4a.ini"
Write-Config $cfg @(@{w = "$sandk\t4\s1"; t = "$sandk\t4\same" }, @{w = "$sandk\t4\s2"; t = "$sandk\t4\same" })
New-Item -ItemType Directory -Force -Path "$sandk\t4\s1", "$sandk\t4\s2", "$sandk\t4\same" | Out-Null
$o = Invoke-Cli $cfg "scan"
if ($o -match "目标目录被多条规则重复使用") { Ok "目标重复被拒绝" } else { Bad "未检出目标重复: $($o.Trim())" }

# 4b 监视目录重叠（父子）
$cfg = "$sandk\t4b.ini"
Write-Config $cfg @(@{w = "$sandk\t4\p"; t = "$sandk\t4\d1" }, @{w = "$sandk\t4\p\child"; t = "$sandk\t4\d2" })
New-Item -ItemType Directory -Force -Path "$sandk\t4\p\child", "$sandk\t4\d1", "$sandk\t4\d2" | Out-Null
$o = Invoke-Cli $cfg "scan"
if ($o -match "监视目录不能相互重叠") { Ok "监视目录重叠被拒绝" } else { Bad "未检出监视重叠: $($o.Trim())" }

# ---------------------------------------------------------------------------
Head "测试5: 目标已存在 → 跳过"
$src = "$sandk\t5\src"; $dst = "$sandk\t5\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path "$dst\A", "$dst\B" | Out-Null
Set-Content "$dst\A\preexisting.txt" "old" -Encoding ascii
$cfg = "$sandk\t5.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
$o = Invoke-Cli $cfg "migrate"
Info ($o.Trim() -split "`n" | Select-Object -Last 1)
if (Test-Path "$dst\A\preexisting.txt") { Ok "既有目标数据未被覆盖" } else { Bad "既有目标被覆盖" }
if (Test-Path "$src\A\sub\a.txt") { Ok "跳过的源目录保持原样" } else { Bad "跳过的源目录被改动" }

# ---------------------------------------------------------------------------
Head "测试6: 占用检测 (锁定文件 → 子任务跳过)"
$src = "$sandk\t6\src"; $dst = "$sandk\t6\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t6.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
# 独占锁定 A\sub\a.txt
$lockPath = "$src\A\sub\a.txt"
$fs = [System.IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'None')
try {
    $o = Invoke-Cli $cfg "migrate"
    Info ($o.Trim())
    if ($o -match "占用") { Ok "报告了占用" } else { Bad "未报告占用" }
    # A 应因占用被跳过：仍是实体目录且文件完整
    $aIt = Get-Item "$src\A" -Force
    if (-not ($aIt.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "被占用子任务保持实体（未半迁移）" } else { Bad "被占用子任务被迁移（不应发生）" }
    if ((Test-Path "$src\A\root.txt") -and (Test-Path "$src\A\sub\a.txt")) { Ok "被占用源目录结构完整" } else { Bad "被占用源目录结构被破坏" }
    # B 不受影响，应已完成迁移
    $bIt = Get-Item "$src\B" -Force
    if ($bIt.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "其他子任务不受影响(独立)" } else { Bad "占用影响了其他子任务" }
}
finally { $fs.Close() }

# ---------------------------------------------------------------------------
Head "测试7: 链接可正常访问 + 恢复"
$src = "$sandk\t7\src"; $dst = "$sandk\t7\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t7.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
Invoke-Cli $cfg "migrate" | Out-Null
$ok = (Get-Content "$src\A\sub\a.txt") -eq "helloA" -and (Test-Path "$src\B\b.bin")
if ($ok) { Ok "经链接访问全部文件正常" } else { Bad "链接访问异常" }
Invoke-Cli $cfg "restore" | Out-Null
$ok2 = (Test-Path "$src\A\sub\a.txt") -and
(Test-Path "$src\B\b.bin") -and
-not ((Get-Item "$src\A" -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)
if ($ok2) { Ok "恢复完整" } else { Bad "恢复不完整" }

# ---------------------------------------------------------------------------
Head "测试8: 配置三处保存 (应用目录/监视目录/目标目录)"
$src = "$sandk\t8\src"; $dst = "$sandk\t8\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t8.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
# 用 CLI migrate 触发一次保存（CLI 会在启动时写配置）
Invoke-Cli $cfg "scan" | Out-Null
$watchCfg = "$src\.dirmv\dirmv.ini"
$targCfg = "$dst\.dirmv\dirmv.ini"
if (Test-Path $watchCfg) { Ok "监视目录已保存配置 ($watchCfg)" } else { Bad "监视目录未保存配置" }
if (Test-Path $targCfg) { Ok "目标目录已保存配置 ($targCfg)" }  else { Bad "目标目录未保存配置" }
$wText = if (Test-Path $watchCfg) { Get-Content $watchCfg -Raw -Encoding UTF8 } else { "" }
if ($wText -match [regex]::Escape($src) -and $wText -match [regex]::Escape($dst)) {
    Ok "监视目录配置内容正确"
}
else { Bad "监视目录配置内容不正确" }

# ---------------------------------------------------------------------------
Head "测试9: 更换目标目录 (数据迁移 + 链接重指向)"
$src = "$sandk\t9\src"; $oldDst = "$sandk\t9\dstOld"; $newDst = "$sandk\t9\dstNew"
New-Tree $src
New-Item -ItemType Directory -Force -Path $oldDst, $newDst | Out-Null
$cfg = "$sandk\t9.ini"
Write-Config $cfg @(@{w = $src; t = $oldDst })
Invoke-Cli $cfg "migrate" | Out-Null
$linkTarget = (Get-Item "$src\A" -Force).Target
if ($linkTarget -and ($linkTarget -join "") -like "*dstOld*") { Ok "迁移后链接指向旧目标" } else { Bad "链接未指向旧目标: $linkTarget" }

# 用 change-target 更换目标目录
$o = & $exe change-target --index 0 --newtarget $newDst --config $cfg 2>&1 | Out-String
Info ($o.Trim() -split "`n" | Select-Object -Last 1)
$linkTarget2 = (Get-Item "$src\A" -Force).Target
if ($linkTarget2 -and ($linkTarget2 -join "") -like "*dstNew*") { Ok "更换目标后链接指向新目标" } else { Bad "链接未指向新目标: $linkTarget2" }
if (Test-Path "$newDst\A\sub\a.txt") { Ok "新目标含数据" } else { Bad "新目标缺少数据" }
if (-not (Test-Path "$oldDst\A")) { Ok "旧目标数据已搬走" } else { Bad "旧目标仍有数据" }
if ((Get-Content "$src\A\sub\a.txt") -eq "helloA") { Ok "经新链接读取正确" } else { Bad "新链接读取错误" }

# ---------------------------------------------------------------------------
Head "测试10: 新增白名单还原已迁移目录"
$src = "$sandk\t10\src"; $dst = "$sandk\t10\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t10.ini"
Write-Config $cfg @(@{w = $src; t = $dst }) ""
Invoke-Cli $cfg "migrate" | Out-Null
$aIt = Get-Item "$src\A" -Force
if ($aIt.Attributes -band [IO.FileAttributes]::ReparsePoint) { Ok "A 已迁移(是链接)" } else { Bad "A 未迁移" }
# 把 A 加入白名单：先还原它
$o = & $exe restore-excluded --index 0 --name "A" --config $cfg 2>&1 | Out-String
Info ($o.Trim() -split "`n" | Select-Object -Last 1)
$aIt2 = Get-Item "$src\A" -Force
if (-not ($aIt2.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "还原后 A 为实体目录" } else { Bad "A 仍为链接" }
if ((Test-Path "$src\A\sub\a.txt") -and (Test-Path "$src\A\root.txt")) { Ok "还原后源目录结构完整" } else { Bad "还原后源目录结构不完整" }
if (-not (Test-Path "$dst\A")) { Ok "目标中 A 已清除" } else { Bad "目标中 A 仍在" }

# ---------------------------------------------------------------------------
Head "测试11: 配置防篡改（HMAC 签名）"
$src = "$sandk\t11\src"; $dst = "$sandk\t11\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t11.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
Invoke-Cli $cfg "scan" | Out-Null   # 触发把带签名配置写入监视目录
$watchCfg = "$src\.dirmv\dirmv.ini"
if (Test-Path $watchCfg) {
    $txt = Get-Content $watchCfg -Raw -Encoding UTF8
    if ($txt -match "#SIG:[0-9a-f]{64}") { Ok "配置含 HMAC-SHA256 签名" } else { Bad "配置缺少签名行" }

    # 未篡改时不应误报签名不匹配（用 import 读取该目录配置验证）
    Remove-Item "$root\build\$Config\dirmv_cli.log" -Force -EA SilentlyContinue
    $impOut = Invoke-Cli $cfg "import" "--dir" $src | Out-String
    if ($impOut -match "签名不通过|签名不匹配") { Bad "未篡改配置被误报签名不通过" }
    else { Ok "未篡改配置签名校验通过（无误报）" }

    # 篡改：加入白名单项
    $tampered = $txt -replace "Exclude=", "Exclude=Hacked|"
    Write-Utf8NoBom $watchCfg $tampered
    Remove-Item "$root\build\$Config\dirmv_cli.log" -Force -EA SilentlyContinue
    Invoke-Cli $watchCfg "scan" | Out-Null
    $log = ""
    if (Test-Path "$root\build\$Config\dirmv_cli.log") { $log = Get-Content "$root\build\$Config\dirmv_cli.log" -Raw -Encoding UTF8 }
    if ($log -match "签名不匹配") { Ok "篡改被检出（签名不匹配）" } else { Bad "篡改未被检出" }
}
else { Bad "监视目录未写入配置" }

# ---------------------------------------------------------------------------
Head "测试12: 扫描接管已有迁移（无记录）"
$src = "$sandk\t12\src"; $dst = "$sandk\t12\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
# 先正常迁移 A、B，然后删除监视目录的配置以模拟「应用没有记录」
$cfg = "$sandk\t12.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
Invoke-Cli $cfg "migrate" | Out-Null
Remove-Item "$src\.dirmv" -Recurse -Force -EA SilentlyContinue
# 用 adopt 扫描接管
$o = & $exe adopt --index 0 --config $cfg 2>&1 | Out-String
Info (($o.Trim()) -split "`n" | Select-Object -First 3 | Out-String)
if ($o -match "新增纳入 2") { Ok "接管识别出 2 个已有迁移" } else { Bad "接管数量不符: $o" }
# 接管后配置里的 Migrated 应含 A、B
$wcfg = Get-Content "$src\.dirmv\dirmv.ini" -Raw -Encoding UTF8
if ($wcfg -match "Migrated=.*A" -and $wcfg -match "Migrated=.*B") { Ok "接管后配置记录了 A、B" } else { Bad "接管后配置未记录" }

# ---------------------------------------------------------------------------
Head "测试13: 外部链接提醒（指向非本目录）"
$src = "$sandk\t13\src"; $dst = "$sandk\t13\dst"; $other = "$sandk\t13\other"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst, "$other\A" | Out-Null
Set-Content "$other\A\foreign.txt" "foreign" -Encoding ascii
# 手动把 src\A 变成指向 other\A 的链接（junction）
if (Test-Path "$src\A") { Remove-Item "$src\A" -Recurse -Force }
cmd /c "mklink /J `"$src\A`" `"$other\A`"" > $null 2>&1
$cfg = "$sandk\t13.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
$o = & $exe adopt --index 0 --config $cfg 2>&1 | Out-String
if ($o -match "不是本规则的目标目录" -and $o -match "other") { Ok "外部链接被提醒" } else { Bad "未提醒外部链接: $o" }

# ---------------------------------------------------------------------------
Head "测试14: 新机器上从目标目录重建链接"
$src = "$sandk\t14\src"; $dst = "$sandk\t14\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfg = "$sandk\t14.ini"
Write-Config $cfg @(@{w = $src; t = $dst })
Invoke-Cli $cfg "migrate" | Out-Null
# 模拟新机器：用全新监视目录（用户名不同 → 路径不同），但目标目录不变
$newWatch = "$sandk\t14\newuser_home"
$o = & $exe rebuild --index 0 --watch $newWatch --config $cfg 2>&1 | Out-String
Info (($o.Trim()) -split "`n" | Select-Object -Last 1)
$aIt = Get-Item "$newWatch\A" -Force -EA SilentlyContinue
if ($aIt -and ($aIt.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "新监视目录已重建 A 链接" } else { Bad "新监视目录未重建链接" }
$bIt = Get-Item "$newWatch\B" -Force -EA SilentlyContinue
if ($bIt -and ($bIt.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "新监视目录已重建 B 链接" } else { Bad "新监视目录未重建 B 链接" }
if ((Get-Content "$newWatch\A\sub\a.txt") -eq "helloA") { Ok "经新链接读取数据正确" } else { Bad "新链接读取数据错误" }

# ---------------------------------------------------------------------------
Head "测试15: 配置目录分发（应用全部/监视目标只含相关规则）"
$srcA = "$sandk\t15\srcA"; $srcB = "$sandk\t15\srcB"
$dstA = "$sandk\t15\dstA"; $dstB = "$sandk\t15\dstB"
foreach ($d in @($srcA, $srcB, $dstA, $dstB)) {
    New-Item -ItemType Directory -Force -Path "$d\X" | Out-Null
    Set-Content "$d\X\f.txt" "x" -Encoding ascii
}
$cfg = "$sandk\t15.ini"
Write-Config $cfg @(@{w = $srcA; t = $dstA }, @{w = $srcB; t = $dstB })
Invoke-Cli $cfg "scan" | Out-Null   # 触发分发
# 监视目录 srcA 的配置应只含 1 条规则（指向 srcA）
$ca = Get-Content "$srcA\.dirmv\dirmv.ini" -Raw -Encoding UTF8
$ruleCountA = ([regex]::Matches($ca, "\[Rule\d+\]")).Count
if ($ruleCountA -eq 1) { Ok "监视目录只含相关规则（1 条）" } else { Bad "监视目录规则数应为 1，实际 $ruleCountA" }
if ($ca -notmatch [regex]::Escape($srcB)) { Ok "监视目录配置不含无关规则" } else { Bad "监视目录配置混入了无关规则" }
# 目标目录 dstA 同样只含 1 条
$da = Get-Content "$dstA\.dirmv\dirmv.ini" -Raw -Encoding UTF8
$ruleCountDA = ([regex]::Matches($da, "\[Rule\d+\]")).Count
if ($ruleCountDA -eq 1) { Ok "目标目录只含相关规则（1 条）" } else { Bad "目标目录规则数应为 1，实际 $ruleCountDA" }

# ---------------------------------------------------------------------------
Head "测试16: 自动/定时迁移只处理正确可用规则（跳过无效规则）"
$srcOk = "$sandk\t16\srcOk"; $dstOk = "$sandk\t16\dstOk"
New-Tree $srcOk
New-Item -ItemType Directory -Force -Path $dstOk | Out-Null
# 第二条规则：监视目录不存在（无效），第三条：未启用
$badWatch = "$sandk\t16\no_such_dir"
$src2 = "$sandk\t16\src2"; $dst2 = "$sandk\t16\dst2"
New-Tree $src2
New-Item -ItemType Directory -Force -Path $dst2 | Out-Null
$cfg = "$sandk\t16.ini"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("[Rule1]")
[void]$sb.AppendLine("WatchDir=$((Get-Item $srcOk).FullName)")
[void]$sb.AppendLine("TargetDir=$((Get-Item $dstOk).FullName)")
[void]$sb.AppendLine("Exclude=Desktop")
[void]$sb.AppendLine("Enabled=1")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("[Rule2]")   # 无效：监视目录不存在
[void]$sb.AppendLine("WatchDir=$badWatch")
[void]$sb.AppendLine("TargetDir=$((Get-Item $dst2).FullName)")
[void]$sb.AppendLine("Enabled=1")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("[Rule3]")   # 未启用
[void]$sb.AppendLine("WatchDir=$((Get-Item $src2).FullName)")
[void]$sb.AppendLine("TargetDir=$((Get-Item $dst2).FullName)")
[void]$sb.AppendLine("Enabled=0")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("[Migrate]")
[void]$sb.AppendLine("LinkType=1")
Write-Utf8NoBom $cfg $sb.ToString()

$o = Invoke-Cli $cfg "migrate"
# 有效规则 Rule1 应完成迁移（A、B）
$aIt = Get-Item "$srcOk\A" -Force -EA SilentlyContinue
if ($aIt -and ($aIt.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Ok "有效规则已迁移（无效规则未阻断）" } else { Bad "有效规则未迁移" }
if ((Get-Content "$srcOk\A\sub\a.txt") -eq "helloA") { Ok "有效规则数据完整" } else { Bad "有效规则数据异常" }
# 无效规则不应破坏流程
if ($o -match "完成：处理") { Ok "迁移流程正常完成（跳过无效规则）" } else { Bad "迁移输出异常: $o" }

# ---------------------------------------------------------------------------
Head "测试17: 从目录配置文件导入规则"
$src = "$sandk\t17\src"; $dst = "$sandk\t17\dst"
New-Tree $src
New-Item -ItemType Directory -Force -Path $dst | Out-Null
$cfgA = "$sandk\t17a.ini"
Write-Config $cfgA @(@{w = $src; t = $dst })
Invoke-Cli $cfgA "scan" | Out-Null   # 触发把配置写到监视/目标目录
# 新配置（干净），从「监视目录配置」导入
$cfgB = "$sandk\t17b.ini"
Write-Config $cfgB @(@{w = "$sandk\t17\other"; t = "$sandk\t17\otherDst" })
$o = & $exe import --dir $src --config $cfgB 2>&1 | Out-String
Info (($o.Trim()) -split "`n" | Select-Object -Last 1)
$importedCfg = Get-Content $cfgB -Raw -Encoding UTF8
if ($importedCfg -match [regex]::Escape($src) -and $importedCfg -match [regex]::Escape($dst)) {
    Ok "从监视目录配置导入成功（含监视与目标目录）"
}
else { Bad "导入后配置未包含导入的规则" }

# 从「目标目录配置」导入 + 用 --watch 重新指定监视目录（模拟换机器）
$newWatch = "$sandk\t17\newuser"
New-Item -ItemType Directory -Force -Path $newWatch | Out-Null
$cfgC = "$sandk\t17c.ini"
Write-Config $cfgC @(@{w = "$sandk\t17\other2"; t = "$sandk\t17\otherDst2" })
$o = & $exe import --dir $dst --watch $newWatch --config $cfgC 2>&1 | Out-String
Info (($o.Trim()) -split "`n" | Select-Object -Last 1)
$importedCfg2 = Get-Content $cfgC -Raw -Encoding UTF8
if ($importedCfg2 -match [regex]::Escape($newWatch)) {
    Ok "从目标目录配置导入并重指定监视目录成功"
}
else { Bad "目标目录导入未应用新监视目录" }

# 导入不存在的目录应失败
$o = & $exe import --dir "$sandk\t17\no_such" --config $cfgC 2>&1 | Out-String
if ($o -match "导入失败" -or $o -match "不存在") { Ok "非法目录导入被拒绝" } else { Bad "非法导入未报错: $o" }

# ---------------------------------------------------------------------------
Head "测试18: 编写测试规则 + 使用测试目录（贴近实际使用）"
# 模拟用户用「新建规则」建立的规则：一个监视目录、一个目标目录、带白名单
$uHome = "$sandk\t18\home"          # 模拟用户主目录
$store = "$sandk\t18\store"        # 模拟迁移存储盘
New-Item -ItemType Directory -Force -Path $store | Out-Null
# 监视目录下：DataX(要迁移) DataY(要迁移) Desktop(白名单) Documents(白名单)
New-Tree $uHome
New-Item -ItemType Directory -Force -Path "$uHome\DataX\logs", "$uHome\DataY\cache" | Out-Null
Set-Content "$uHome\DataX\logs\x.log" "xdata" -Encoding ascii
Set-Content "$uHome\DataY\cache\y.bin" "ydata" -Encoding ascii
# 规则1：迁移 home -> store，白名单 Desktop|Documents
$cfg = "$sandk\t18.ini"
Write-Config $cfg @(@{w = $uHome; t = $store }) "Desktop|Documents"

# (1) scan：应列出可迁移的一级目录（A、B、DataX、DataY），不含白名单
$o = Invoke-Cli $cfg "scan"
if ($o -match "DataX" -and $o -match "DataY" -and $o -notmatch "Desktop") {
    Ok "测试目录：scan 正确列出可迁移项并排除白名单"
}
else { Bad "scan 结果不符: $o" }

# (2) migrate：全部迁移并建立链接
$o = Invoke-Cli $cfg "migrate"
Info (($o.Trim()) -split "`n" | Select-Object -Last 1)
$okAll = $true
foreach ($d in @("A", "B", "DataX", "DataY")) {
    $it = Get-Item "$uHome\$d" -Force -EA SilentlyContinue
    if (-not ($it -and ($it.Attributes -band [IO.FileAttributes]::ReparsePoint))) { $okAll = $false }
}
if ($okAll) { Ok "测试目录：全部非白名单目录已迁移为链接" } else { Bad "部分目录未迁移" }
if ((Get-Content "$uHome\DataX\logs\x.log") -eq "xdata") { Ok "测试目录：经链接读取 DataX 正常" } else { Bad "DataX 读取异常" }
if (Test-Path "$uHome\Desktop\d.txt" ) { Ok "测试目录：白名单 Desktop 保留原样" } else { Bad "白名单目录被改动" }
if (Test-Path "$store\DataY\cache\y.bin") { Ok "测试目录：目标目录含迁移数据" } else { Bad "目标目录缺少数据" }

# (3) 恢复：全部还原
$o = Invoke-Cli $cfg "restore"
Info (($o.Trim()) -split "`n" | Select-Object -Last 1)
$restored = $true
foreach ($d in @("A", "B", "DataX", "DataY")) {
    $it = Get-Item "$uHome\$d" -Force -EA SilentlyContinue
    if (-not $it -or ($it.Attributes -band [IO.FileAttributes]::ReparsePoint)) { $restored = $false }
}
if ($restored) { Ok "测试目录：全部还原为实体目录" } else { Bad "还原不完整" }
if ((Get-Content "$uHome\DataY\cache\y.bin") -eq "ydata") { Ok "测试目录：还原后数据完整" } else { Bad "还原后数据丢失" }

# ===========================================================================
Head "结果"
Write-Host "  通过: $pass   失败: $fail" -ForegroundColor $(if ($fail -eq 0) { "Green" } else { "Red" })
if ($fail -eq 0) { exit 0 } else { exit 1 }
