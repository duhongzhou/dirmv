# 合并多个单尺寸 .ico 为一个多尺寸 .ico（ICO 文件格式：ICONDIR + 多个 ICONDIRENTRY + 图像数据）
param(
    [Parameter(Mandatory = $true)][string[]]$Inputs,
    [Parameter(Mandatory = $true)][string]$Output
)

$ErrorActionPreference = "Stop"

# 读取每个 ico 的全部图像条目（含各自可能的多尺寸）
$images = @()  # 每项: @{ w; h; colors; planes; bpp; data(byte[]) }
foreach ($path in $Inputs) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 6) { throw "无效 ico: $path" }
    $type = [BitConverter]::ToUInt16($bytes, 2)
    $count = [BitConverter]::ToUInt16($bytes, 4)
    if ($type -ne 1) { throw "不是图标文件: $path" }
    for ($i = 0; $i -lt $count; $i++) {
        $off = 6 + $i * 16
        $w = $bytes[$off]
        $h = $bytes[$off + 1]
        $col = $bytes[$off + 2]
        $pl = [BitConverter]::ToUInt16($bytes, $off + 4)
        $bpp = [BitConverter]::ToUInt16($bytes, $off + 6)
        $sz = [BitConverter]::ToUInt32($bytes, $off + 8)
        $dof = [BitConverter]::ToUInt32($bytes, $off + 12)
        $data = New-Object byte[] $sz
        [Array]::Copy($bytes, $dof, $data, 0, $sz)
        $images += , @{ w = $w; h = $h; colors = $col; planes = $pl; bpp = $bpp; data = $data }
    }
}

# 按尺寸从大到小排序（Windows 惯例），并去重（保留最大）
$images = $images | Sort-Object -Property @{ Expression = { $_.w } } -Descending

# 写出合并文件
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([UInt16]0)                 # reserved
$bw.Write([UInt16]1)                 # type=icon
$bw.Write([UInt16]$images.Count)     # count

$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $bw.Write([byte]$img.w)
    $bw.Write([byte]$img.h)
    $bw.Write([byte]$img.colors)
    $bw.Write([byte]0)               # reserved
    $bw.Write([UInt16]$img.planes)
    $bw.Write([UInt16]$img.bpp)
    $bw.Write([UInt32]$img.data.Length)
    $bw.Write([UInt32]$offset)
    $offset += $img.data.Length
}
foreach ($img in $images) {
    $bw.Write($img.data)
}
$bw.Flush()
[System.IO.File]::WriteAllBytes($Output, $ms.ToArray())
$bw.Dispose(); $ms.Dispose()

Write-Output ("已合并 {0} 个图像 -> {1}" -f $images.Count, $Output)
Write-Output ("尺寸: " + (($images | ForEach-Object { "$($_.w)x$($_.h)" }) -join ", "))
