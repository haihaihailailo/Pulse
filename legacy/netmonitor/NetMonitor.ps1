<#
  网络监测 —— 断网恢复提醒（托盘后台常驻）
  ------------------------------------------
  每隔几秒向一个极小的 HTTP 探测地址发请求（会走系统代理 / 加速器，测的是“真正能不能上网”）：
    - 收到 HTTP 响应且延迟低于不可用阈值 = 在线；
    - 连续无响应或延迟过高达到次数阈值 = 断网；
    - 断网后恢复时：右下角弹出提醒卡片 + 提示音，并写入日志。

  安装（复制到 %LOCALAPPDATA%\NetMonitor，生成图标和桌面快捷方式，并启动）：
    powershell -ExecutionPolicy Bypass -File .\NetMonitor.ps1 -Install
  卸载：
    powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\NetMonitor\NetMonitor.ps1" -Uninstall
#>
param(
    [switch]$Install,
    [switch]$Uninstall
)

# ================= 配置 =================
# 探测地址：先只请求第一个，失败了才并行请求其余的（省流量）
$CheckUrls = @(
    'http://www.msftconnecttest.com/connecttest.txt',
    'http://www.gstatic.com/generate_204',
    'http://connect.rom.miui.com/generate_204'
)
$IntervalSeconds = 5    # 检测间隔
$FailThreshold   = 3    # 连续不可用几次判定为断网
$TimeoutSeconds  = 4    # 单次请求超时
$SlowMs          = 300  # 延迟达到此值显示较慢
$UnusableMs      = 1000 # 延迟达到此值记为不可用
# ========================================

$AppName    = '网络监测'
$InstallDir = Join-Path $env:LOCALAPPDATA 'NetMonitor'
$LogFile    = Join-Path $PSScriptRoot 'NetMonitor.log'
$DesktopLnk = Join-Path ([Environment]::GetFolderPath('Desktop')) '网络监测.lnk'
$StartupLnk = Join-Path ([Environment]::GetFolderPath('Startup')) '网络监测.lnk'

Add-Type -AssemblyName System.Windows.Forms, System.Drawing, System.Net.Http
Add-Type -Namespace NetMon -Name Native -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();'
[void][NetMon.Native]::SetProcessDPIAware()
[System.Windows.Forms.Application]::EnableVisualStyles()
$g0 = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $Scale = $g0.DpiX / 96; $g0.Dispose()

# ---------------- 绘图 ----------------
function C([string]$Hex, [int]$Alpha = 255) {
    [System.Drawing.Color]::FromArgb($Alpha, [System.Drawing.ColorTranslator]::FromHtml($Hex))
}

$Palettes = @{
    online   = @('#5EEAD4', '#38BDF8')   # 薄荷 → 天蓝
    slow     = @('#FCD34D', '#F59E0B')   # 琥珀
    offline  = @('#FDA4AF', '#F43F5E')   # 玫瑰
    checking = @('#E2E8F0', '#94A3B8')   # 灰
}

function New-RoundedPath([single]$X, [single]$Y, [single]$W, [single]$H, [single]$R) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $R * 2
    $p.AddArc($X, $Y, $d, $d, 180, 90)
    $p.AddArc($X + $W - $d, $Y, $d, $d, 270, 90)
    $p.AddArc($X + $W - $d, $Y + $H - $d, $d, $d, 0, 90)
    $p.AddArc($X, $Y + $H - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    $p
}

# 图标：深色渐变圆角底板 + 渐变 Wi-Fi 弧线。4 倍超采样后缩小，边缘更细腻。
function New-AppBitmap([int]$Size, [string]$State = 'online') {
    $S = $Size * 4; $k = $S / 256.0
    $pal = $Palettes[$State]
    $big = New-Object System.Drawing.Bitmap $S, $S
    $g = [System.Drawing.Graphics]::FromImage($big)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # 底板
    $bg = New-RoundedPath (8 * $k) (8 * $k) (240 * $k) (240 * $k) (64 * $k)
    $full = New-Object System.Drawing.RectangleF 0, 0, $S, $S
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList $full, (C '#273449'), (C '#0B1120'), ([single]90)
    $g.FillPath($bgBrush, $bg)

    # 顶部柔光
    $g.SetClip($bg)
    $glossRect = New-Object System.Drawing.RectangleF 0, (8 * $k), $S, (120 * $k)
    $gloss = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList $glossRect, (C '#FFFFFF' 26), (C '#FFFFFF' 0), ([single]90)
    $g.FillRectangle($gloss, $glossRect)
    $g.ResetClip()

    # 细描边
    $edge = New-Object System.Drawing.Pen (C '#FFFFFF' 28), ([single](2.5 * $k))
    $g.DrawPath($edge, $bg)

    # Wi-Fi 弧线
    $cx = 128 * $k; $cy = 178 * $k
    if ($Size -le 24) { $radii = 52, 102; $w = 30 * $k; $dot = 21 * $k }
    else              { $radii = 38, 74, 110; $w = 21 * $k; $dot = 16 * $k }
    $arcRect = New-Object System.Drawing.RectangleF ($cx - 110 * $k), ($cy - 110 * $k), (220 * $k), (130 * $k)
    $arcBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList $arcRect, (C $pal[0]), (C $pal[1]), ([single]60)

    if ($Size -ge 48) {
        # 发光
        $glow = New-Object System.Drawing.Pen (C $pal[1] 45), ([single]($w * 2.2))
        $glow.StartCap = 'Round'; $glow.EndCap = 'Round'
        foreach ($r in $radii) { $g.DrawArc($glow, [single]($cx - $r * $k), [single]($cy - $r * $k), [single](2 * $r * $k), [single](2 * $r * $k), [single]225, [single]90) }
        $g.FillEllipse((New-Object System.Drawing.SolidBrush (C $pal[1] 45)), [single]($cx - $dot * 2), [single]($cy - $dot * 2), [single]($dot * 4), [single]($dot * 4))
    }
    $pen = New-Object System.Drawing.Pen $arcBrush, ([single]$w)
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    foreach ($r in $radii) { $g.DrawArc($pen, [single]($cx - $r * $k), [single]($cy - $r * $k), [single](2 * $r * $k), [single](2 * $r * $k), [single]225, [single]90) }
    $g.FillEllipse($arcBrush, [single]($cx - $dot), [single]($cy - $dot), [single]($dot * 2), [single]($dot * 2))
    $g.Dispose()

    $bmp = New-Object System.Drawing.Bitmap $Size, $Size
    $g2 = [System.Drawing.Graphics]::FromImage($bmp)
    $g2.InterpolationMode = 'HighQualityBicubic'; $g2.PixelOffsetMode = 'HighQuality'; $g2.CompositingQuality = 'HighQuality'
    $g2.DrawImage($big, 0, 0, $Size, $Size)
    $g2.Dispose(); $big.Dispose()
    $bmp
}

# 多尺寸 .ico（PNG 压缩条目）
function Save-Ico([string]$Path, [int[]]$Sizes) {
    $pngs = New-Object System.Collections.Generic.List[byte[]]
    foreach ($s in $Sizes) {
        $b = New-AppBitmap $s 'online'
        $ms = New-Object System.IO.MemoryStream
        $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $pngs.Add($ms.ToArray()); $b.Dispose()
    }
    $fs = [System.IO.File]::Create($Path)
    $bw = New-Object System.IO.BinaryWriter $fs
    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$Sizes.Count)
    $offset = 6 + 16 * $Sizes.Count
    for ($i = 0; $i -lt $Sizes.Count; $i++) {
        $dim = [byte]($Sizes[$i] % 256)
        $bw.Write($dim); $bw.Write($dim); $bw.Write([byte]0); $bw.Write([byte]0)
        $bw.Write([uint16]1); $bw.Write([uint16]32)
        $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
        $offset += $pngs[$i].Length
    }
    foreach ($p in $pngs) { $bw.Write($p) }
    $bw.Close()
}

# ---------------- 安装 / 卸载 ----------------
function Stop-OtherInstances {
    Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
        Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like '*NetMonitor.ps1*' -and $_.CommandLine -notlike '*-Install*' -and $_.CommandLine -notlike '*-Uninstall*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function New-AppShortcut([string]$Path) {
    $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut($Path)
    $lnk.TargetPath = Join-Path $env:WINDIR 'System32\wscript.exe'
    $lnk.Arguments = '"' + (Join-Path $InstallDir 'NetMonitor.vbs') + '"'
    $lnk.WorkingDirectory = $InstallDir
    $lnk.IconLocation = (Join-Path $InstallDir 'NetMonitor.ico') + ',0'
    $lnk.Description = '断网恢复提醒'
    $lnk.Save()
}

if ($Install) {
    Stop-OtherInstances
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $dst = Join-Path $InstallDir 'NetMonitor.ps1'
    if ($PSCommandPath -ne $dst) { Copy-Item $PSCommandPath $dst -Force }
    Save-Ico (Join-Path $InstallDir 'NetMonitor.ico') @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    # 用 VBS 启动，完全不闪黑窗口
    $vbs = 'CreateObject("WScript.Shell").Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ""' + $dst + '""", 0, False'
    Set-Content -Path (Join-Path $InstallDir 'NetMonitor.vbs') -Value $vbs -Encoding Unicode
    New-AppShortcut $DesktopLnk
    if (Test-Path $StartupLnk) { New-AppShortcut $StartupLnk }
    Start-Process (Join-Path $env:WINDIR 'System32\wscript.exe') -ArgumentList ('"' + (Join-Path $InstallDir 'NetMonitor.vbs') + '"')
    Write-Host "已安装到 $InstallDir，桌面快捷方式已创建，程序已在托盘启动。"
    return
}

if ($Uninstall) {
    Stop-OtherInstances
    Remove-Item $DesktopLnk, $StartupLnk -Force -ErrorAction SilentlyContinue
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '已卸载。'
    return
}

# ---------------- 主程序 ----------------
$mutex = New-Object System.Threading.Mutex($false, 'Local\NetMonitor-Tray')
if (-not $mutex.WaitOne(0)) {
    [void][System.Windows.Forms.MessageBox]::Show('网络监测已经在后台运行了，看任务栏右下角的托盘图标。', $AppName)
    return
}

function Write-Log([string]$Message) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message
    try { Add-Content -Path $LogFile -Value $line -Encoding UTF8 } catch { }
}

function Format-Duration([TimeSpan]$ts) {
    if ($ts.TotalHours -ge 1) { return '{0} 小时 {1} 分' -f [int][math]::Floor($ts.TotalHours), $ts.Minutes }
    if ($ts.TotalMinutes -ge 1) { return '{0} 分 {1} 秒' -f $ts.Minutes, $ts.Seconds }
    return '{0} 秒' -f $ts.Seconds
}

# 右下角提醒卡片（点任意位置关闭，不会自动消失）
function Show-Card([string]$Title, [string]$Body, [string]$State) {
    $s = $Scale
    $W = [int](372 * $s); $H = [int](108 * $s)
    $f = New-Object System.Windows.Forms.Form
    $f.FormBorderStyle = 'None'; $f.ShowInTaskbar = $false; $f.TopMost = $true
    $f.StartPosition = 'Manual'; $f.BackColor = C '#0F172A'
    $f.ClientSize = New-Object System.Drawing.Size $W, $H
    $wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $f.Location = New-Object System.Drawing.Point ($wa.Right - $W - [int](20 * $s)), ($wa.Bottom - $H - [int](20 * $s))
    $f.Region = New-Object System.Drawing.Region (New-RoundedPath 0 0 $W $H (18 * $s))
    $f.Add_Paint({
        $_.Graphics.SmoothingMode = 'AntiAlias'
        $pen = New-Object System.Drawing.Pen (C '#334155'), ([single]1.5)
        $_.Graphics.DrawPath($pen, (New-RoundedPath 0.75 0.75 ($this.ClientSize.Width - 1.5) ($this.ClientSize.Height - 1.5) (18 * $Scale)))
    })

    $icon = [int](56 * $s)
    $pic = New-Object System.Windows.Forms.PictureBox
    $pic.Image = New-AppBitmap $icon $State
    $pic.Size = New-Object System.Drawing.Size $icon, $icon
    $pic.Location = New-Object System.Drawing.Point ([int](22 * $s)), ([int](($H - $icon) / 2))

    $t = New-Object System.Windows.Forms.Label
    $t.Text = $Title; $t.AutoSize = $true; $t.ForeColor = C '#F8FAFC'
    $t.Font = New-Object System.Drawing.Font 'Microsoft YaHei UI', 13, ([System.Drawing.FontStyle]::Bold)
    $t.Location = New-Object System.Drawing.Point ([int](94 * $s)), ([int](26 * $s))

    $b = New-Object System.Windows.Forms.Label
    $b.Text = $Body; $b.AutoSize = $true; $b.ForeColor = C '#94A3B8'
    $b.Font = New-Object System.Drawing.Font 'Microsoft YaHei UI', 9.5
    $b.Location = New-Object System.Drawing.Point ([int](95 * $s)), ([int](60 * $s))

    $x = New-Object System.Windows.Forms.Label
    $x.Text = '×'; $x.AutoSize = $true; $x.ForeColor = C '#64748B'; $x.Cursor = 'Hand'
    $x.Font = New-Object System.Drawing.Font 'Segoe UI', 12
    $x.Location = New-Object System.Drawing.Point ($W - [int](30 * $s)), ([int](6 * $s))

    $f.Controls.AddRange(@($pic, $t, $b, $x))
    $close = { $frm = if ($this -is [System.Windows.Forms.Form]) { $this } else { $this.FindForm() }; $frm.Close() }
    foreach ($c in @($f, $pic, $t, $b, $x)) { $c.Add_Click($close) }
    $f.Add_FormClosed({ $this.Dispose() })
    $f.Show()
}

function Show-Alert([string]$Title, [string]$Body, [string]$State) {
    Show-Card $Title $Body $State
    try {
        $wav = Join-Path $env:WINDIR 'Media\Windows Notify System Generic.wav'
        if (Test-Path $wav) { (New-Object System.Media.SoundPlayer $wav).Play() }
        else { [System.Media.SystemSounds]::Asterisk.Play() }
    } catch { }
}

# 托盘
$TraySize = [System.Windows.Forms.SystemInformation]::SmallIconSize.Width
$Icons = @{}; $Dots = @{}
foreach ($st in 'online', 'slow', 'offline', 'checking') {
    $Icons[$st] = [System.Drawing.Icon]::FromHandle((New-AppBitmap $TraySize $st).GetHicon())
    $Dots[$st]  = New-AppBitmap ([int](16 * $Scale)) $st
}

$tray = New-Object System.Windows.Forms.NotifyIcon
$menu = New-Object System.Windows.Forms.ContextMenuStrip
$menu.Font = New-Object System.Drawing.Font 'Microsoft YaHei UI', 9
$miStatus = $menu.Items.Add('正在检测…'); $miStatus.Enabled = $false
[void]$menu.Items.Add((New-Object System.Windows.Forms.ToolStripSeparator))
$miTest = $menu.Items.Add('测试提醒效果')
$miTest.Add_Click({ Show-Alert '网络已恢复' ('{0} 恢复连接 · 这是一条测试提醒' -f (Get-Date -Format 'HH:mm')) 'online' })
$miLog = $menu.Items.Add('打开日志')
$miLog.Add_Click({ if (Test-Path $LogFile) { Start-Process notepad.exe -ArgumentList ('"' + $LogFile + '"') } })
$miStartup = New-Object System.Windows.Forms.ToolStripMenuItem '开机自动启动'
$miStartup.Checked = Test-Path $StartupLnk
$miStartup.CheckOnClick = $true
$miStartup.Add_CheckedChanged({
    if ($this.Checked) { New-AppShortcut $StartupLnk } else { Remove-Item $StartupLnk -Force -ErrorAction SilentlyContinue }
})
[void]$menu.Items.Add($miStartup)
[void]$menu.Items.Add((New-Object System.Windows.Forms.ToolStripSeparator))
$miExit = $menu.Items.Add('退出')
$miExit.Add_Click({ $tray.Visible = $false; [System.Windows.Forms.Application]::Exit() })
$tray.ContextMenuStrip = $menu
$tray.Add_MouseClick({ if ($_.Button -eq 'Left') { $tray.ShowBalloonTip(3000, $AppName, $script:StatusText, 'None') } })

function Set-Status([string]$State, [string]$Text) {
    $script:StatusText = $Text
    $tray.Icon = $Icons[$State]
    $tray.Text = "$AppName · $Text"
    $miStatus.Text = $Text
    $miStatus.Image = $Dots[$State]
}

# 检测（异步请求 + 定时器轮询，界面不会卡）
$handler = New-Object System.Net.Http.HttpClientHandler
$handler.AllowAutoRedirect = $false
$http = New-Object System.Net.Http.HttpClient $handler
$http.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)
$http.DefaultRequestHeaders.ConnectionClose = $true

$script:tasks = $null
$script:stage = 0
$script:watch = $null
$script:nextCheck = Get-Date
$script:failCount = 0
$script:offlineSince = $null

function Start-Probe([string[]]$Urls) {
    $script:tasks = @(foreach ($u in $Urls) { $http.GetAsync($u, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead) })
}

function Set-Result([bool]$Ok, [long]$Ms) {
    # 请求失败或延迟过高都算不可用，只有低于不可用阈值才算恢复
    if ($Ok -and $Ms -lt $UnusableMs) {
        if ($script:failCount -ge $FailThreshold) {
            $dur = Format-Duration ((Get-Date) - $script:offlineSince)
            Write-Log "网络已恢复，断网时长 $dur"
            Show-Alert '网络已恢复' ('{0} 恢复连接 · 本次断网 {1}' -f (Get-Date -Format 'HH:mm'), $dur) 'online'
        } elseif ($script:failCount -gt 0) {
            Write-Log "短暂波动（失败 $($script:failCount) 次）后恢复"
        }
        $script:failCount = 0
        $script:offlineSince = $null
        if ($Ms -ge $SlowMs) { Set-Status 'slow' ('较慢 · {0} ms' -f $Ms) }
        else { Set-Status 'online' ('在线 · {0} ms' -f $Ms) }
    } else {
        $script:failCount++
        if ($script:failCount -eq 1) { $script:offlineSince = Get-Date }
        if ($script:failCount -eq $FailThreshold) {
            $reason = if ($Ok) { '延迟过高（{0} ms）' -f $Ms } else { '无响应' }
            Write-Log "网络断开：$reason"
        }
        if ($script:failCount -ge $FailThreshold) {
            if ($Ok) { Set-Status 'offline' ('网络不可用 · {0} ms' -f $Ms) }
            else { Set-Status 'offline' ('已断开 · 自 {0}' -f $script:offlineSince.ToString('HH:mm:ss')) }
        } elseif ($Ok) {
            Set-Status 'slow' ('较慢 · {0} ms' -f $Ms)
        }
    }
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 250
$timer.Add_Tick({
    try {
        if ($script:tasks) {
            $ok = $false; $allDone = $true
            foreach ($t in $script:tasks) {
                if (-not $t.IsCompleted) { $allDone = $false }
                elseif ($t.Status -eq 'RanToCompletion') { $ok = $true }   # 收到 HTTP 响应，延迟交给 Set-Result 判定
            }
            if ($ok -or $allDone) {
                foreach ($t in $script:tasks) { if ($t.Status -eq 'RanToCompletion') { $t.Result.Dispose() } }
                $script:tasks = $null
                if ($ok) { Set-Result $true $script:watch.ElapsedMilliseconds }
                elseif ($script:stage -eq 1 -and $CheckUrls.Count -gt 1) {
                    $script:stage = 2
                    $script:watch = [System.Diagnostics.Stopwatch]::StartNew()   # 备用地址重新计时，避免把第一个地址的超时算进延迟
                    Start-Probe $CheckUrls[1..($CheckUrls.Count - 1)]
                } else { Set-Result $false 0 }
            }
        } elseif ((Get-Date) -ge $script:nextCheck) {
            $script:nextCheck = (Get-Date).AddSeconds($IntervalSeconds)
            $script:stage = 1
            $script:watch = [System.Diagnostics.Stopwatch]::StartNew()
            Start-Probe @($CheckUrls[0])
        }
    } catch {
        Write-Log "检测出错：$($_.Exception.Message)"
        $script:tasks = $null
    }
})

Write-Log '启动监测'
Set-Status 'checking' '正在检测…'
$tray.Visible = $true
$timer.Start()
[System.Windows.Forms.Application]::Run()

$timer.Stop()
$tray.Visible = $false
$tray.Dispose()
$http.Dispose()
Write-Log '退出监测'
$mutex.ReleaseMutex()
