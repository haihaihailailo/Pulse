param([string]$Out = (Join-Path $PSScriptRoot '..\pulse.ico'), [string]$PreviewDir = '')
# 生成 Pulse 图标（深蓝圆角底 + 半圆仪表盘 + 指针），多尺寸 PNG 压缩条目写入 .ico
Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256

function New-Color([string]$hex) { [System.Drawing.ColorTranslator]::FromHtml($hex) }

function Render([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $k = $size / 32.0
    $g.ScaleTransform($k, $k)

    # 底：圆角方块
    $bg = New-Object System.Drawing.Drawing2D.GraphicsPath
    $r = 7.0; $d = 2 * $r; $x = 1.0; $y = 1.0; $w = 30.0
    $bg.AddArc($x, $y, $d, $d, 180, 90); $bg.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $bg.AddArc($x + $w - $d, $y + $w - $d, $d, $d, 0, 90); $bg.AddArc($x, $y + $w - $d, $d, $d, 90, 90)
    $bg.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush (New-Color '#151c2f')), $bg)

    # 表盘：圆心 (16, 21)，半径 9；小尺寸加粗线条保证清晰
    $stroke = if ($size -le 24) { 4.6 } elseif ($size -le 32) { 4.0 } else { 3.6 }
    $cx = 16.0
    $cy = if ($size -le 24) { 22.5 } else { 21.5 }
    $rad = if ($size -le 24) { 11.0 } else { 10.0 }
    $rect = New-Object System.Drawing.RectangleF ($cx - $rad), ($cy - $rad), (2 * $rad), (2 * $rad)
    $track = New-Object System.Drawing.Pen (New-Color '#2a3550'), $stroke
    $track.StartCap = 'Round'; $track.EndCap = 'Round'
    $g.DrawArc($track, $rect, 180, 180)
    $value = New-Object System.Drawing.Pen (New-Color '#38bdf8'), $stroke
    $value.StartCap = 'Round'; $value.EndCap = 'Round'
    $g.DrawArc($value, $rect, 180, 125)

    # 指针与轴心
    $angle = (180 + 125) * [Math]::PI / 180
    $len = $rad * 0.72
    $needleWidth = if ($size -le 24) { 3.0 } else { 2.4 }
    $needle = New-Object System.Drawing.Pen (New-Color '#e2e8f0'), $needleWidth
    $needle.StartCap = 'Round'; $needle.EndCap = 'Round'
    $g.DrawLine($needle, [single]$cx, [single]$cy, [single]($cx + $len * [Math]::Cos($angle)), [single]($cy + $len * [Math]::Sin($angle)))
    $hub = if ($size -le 24) { 2.6 } else { 2.2 }
    $g.FillEllipse((New-Object System.Drawing.SolidBrush (New-Color '#e2e8f0')), [single]($cx - $hub), [single]($cy - $hub), [single](2 * $hub), [single](2 * $hub))

    $g.Dispose()
    return $bmp
}

$images = foreach ($s in $sizes) {
    $bmp = Render $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($PreviewDir) { $bmp.Save((Join-Path $PreviewDir "icon_$s.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
}

# ICO：ICONDIR + ICONDIRENTRY[n] + PNG 数据
$ico = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ico
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $dim = if ($img.Size -ge 256) { 0 } else { $img.Size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$img.Data.Length); $bw.Write([uint32]$offset)
    $offset += $img.Data.Length
}
foreach ($img in $images) { $bw.Write($img.Data) }
$bw.Flush()
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Out), $ico.ToArray())
"wrote $Out ($($ico.Length) bytes, $($images.Count) sizes)"
