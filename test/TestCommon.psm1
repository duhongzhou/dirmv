# ---------------------------------------------------------------------------
# TestCommon.psm1 - dirMover 测试公共库
#   · 安全护栏：所有路径必须在 test\sandbox 之下，越界即拒绝
#   · 生成「测试规则文件」（带 HMAC 签名由应用写入，脚本只准备草稿）
#   · 提供 CLI 调用封装
# ---------------------------------------------------------------------------
Set-StrictMode -Version Latest

# --- 路径解析 -------------------------------------------------------------
function Get-ProjectRoot {
    param([string]$ScriptRoot)
    return (Split-Path -Parent $ScriptRoot)
}

# 断言路径位于 $Sandbox 之下（不区分大小写，含规范化），否则抛出
# 允许的额外例外根（如跨盘测试目录 F:\...）通过 -ExtraRoots 传入
function Assert-InSandbox {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Sandbox,
        [string[]]$ExtraRoots
    )
    $full = [System.IO.Path]::GetFullPath($Path)
    $roots = @($Sandbox) + @($ExtraRoots)
    foreach ($root in $roots) {
        if (-not $root) { continue }
        $r = [System.IO.Path]::GetFullPath($root)
        if (-not $r.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
            $r += [System.IO.Path]::DirectorySeparatorChar
        }
        $a = $full.TrimEnd('\')
        $b = $r.TrimEnd('\')
        if ($a -eq $b -or $a.StartsWith($r, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $full
        }
    }
    throw "安全护栏：路径超出测试沙箱，已拒绝操作 -> $full (沙箱: $Sandbox；允许: $($ExtraRoots -join ', '))"
}

# --- 文件/目录工具 --------------------------------------------------------
function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

# 生成「测试规则文件」：$rules = @(@{w=..;t=..;ex=..;en=1}, ...)
function New-TestRuleFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][array]$Rules,
        [Parameter(Mandatory = $true)][string]$Sandbox,
        [string[]]$ExtraRoots
    )
    Assert-InSandbox -Path $Path -Sandbox $Sandbox | Out-Null
    $sb = New-Object System.Text.StringBuilder
    $i = 1
    foreach ($r in $Rules) {
        Assert-InSandbox -Path $r.w -Sandbox $Sandbox -ExtraRoots $ExtraRoots | Out-Null
        if ($r.t) { Assert-InSandbox -Path $r.t -Sandbox $Sandbox -ExtraRoots $ExtraRoots | Out-Null }
        $ex = if ($r.ContainsKey('ex')) { $r.ex } else { "Desktop|Documents" }
        $en = if ($r.ContainsKey('en')) { $r.en } else { 1 }
        [void]$sb.AppendLine("[Rule$i]")
        [void]$sb.AppendLine("WatchDir=$($r.w)")
        [void]$sb.AppendLine("TargetDir=$($r.t)")
        [void]$sb.AppendLine("Exclude=$ex")
        [void]$sb.AppendLine("Enabled=$en")
        [void]$sb.AppendLine("")
        $i++
    }
    [void]$sb.AppendLine("[Migrate]")
    [void]$sb.AppendLine("LinkType=1")
    [void]$sb.AppendLine("MoveMode=1")
    [void]$sb.AppendLine("SkipExisting=1")
    [void]$sb.AppendLine("WorkerThreads=2")
    [void]$sb.AppendLine("EnableWatcher=0")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("[Lock]")
    [void]$sb.AppendLine("Detect=1")
    [void]$sb.AppendLine("RetryDelayMinutes=5")
    Write-Utf8NoBom -Path $Path -Text $sb.ToString()
    return $Path
}

# 创建测试目录树：$base 下 A\sub\a.txt, B\b.bin, Desktop\d.txt
function New-TestTree {
    param(
        [Parameter(Mandatory = $true)][string]$Base,
        [Parameter(Mandatory = $true)][string]$Sandbox
    )
    Assert-InSandbox -Path $Base -Sandbox $Sandbox | Out-Null
    New-Item -ItemType Directory -Force -Path "$Base\A\sub", "$Base\B", "$Base\Desktop" | Out-Null
    Set-Content -Path "$Base\A\sub\a.txt" -Value "helloA" -Encoding ascii
    Set-Content -Path "$Base\A\root.txt"  -Value "rootA"  -Encoding ascii
    Set-Content -Path "$Base\B\b.bin"     -Value "dataB"  -Encoding ascii
    Set-Content -Path "$Base\Desktop\d.txt" -Value "desk" -Encoding ascii
}

# 调用 CLI，返回输出文本
function Invoke-Cli {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string[]]$Args,
        [string]$Sandbox
    )
    if ($Sandbox) { Assert-InSandbox -Path $ConfigPath -Sandbox $Sandbox | Out-Null }
    $all = @($Args) + @("--config", $ConfigPath)
    $out = & $Exe @all 2>&1 | Out-String
    return $out
}

Export-ModuleMember -Function *
