# ---------------------------------------------------------------------------
# run_all_tests.ps1 - 一键运行全部测试
#   1) clitest.ps1   —— 用「测试规则文件」通过 CLI 驱动，验证各项功能
#   2) run_tests.ps1 —— 原有综合功能测试（57 项）
#
#   安全：全部在 test\sandbox 下操作，不碰真实目录。
#   用法：powershell -ExecutionPolicy Bypass -File test\run_all_tests.ps1
# ---------------------------------------------------------------------------
param([string]$Config = "Release")

$ErrorActionPreference = "Continue"

$suites = @(
    @{ name = "CLI 全功能测试 (clitest)"; file = "clitest.ps1" },
    @{ name = "综合功能测试 (run_tests)"; file = "run_tests.ps1" },
    @{ name = "迁移安全性测试 (safety_test)"; file = "safety_test.ps1" }
)

$results = @()
foreach ($s in $suites) {
    Write-Host "`n############### $($s.name) ###############" -ForegroundColor Magenta
    $f = Join-Path $PSScriptRoot $s.file
    & powershell -ExecutionPolicy Bypass -File $f -Config $Config
    $results += [pscustomobject]@{ Suite = $s.name; ExitCode = $LASTEXITCODE }
}

Write-Host "`n================= 汇总 =================" -ForegroundColor Magenta
$bad = 0
foreach ($r in $results) {
    $color = if ($r.ExitCode -eq 0) { "Green" } else { "Red"; $bad++ }
    Write-Host ("  {0,-36} {1}" -f $r.Suite, $(if ($r.ExitCode -eq 0) { "PASS" } else { "FAIL($($r.ExitCode))" })) -ForegroundColor $color
}

# 清理沙箱
$sandk = Join-Path $PSScriptRoot "sandbox"
if (Test-Path $sandk) { Remove-Item $sandk -Recurse -Force -ErrorAction SilentlyContinue }

if ($bad -eq 0) { Write-Host "`n全部测试套件通过。" -ForegroundColor Green; exit 0 }
else { Write-Host "`n有 $bad 个套件失败。" -ForegroundColor Red; exit 1 }
