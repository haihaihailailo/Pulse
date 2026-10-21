#Requires -Version 5.1
<#
.SYNOPSIS
    卸载当前用户安装的 Pulse。
.DESCRIPTION
    结束 Pulse 进程，删除开机自启、开始菜单和桌面快捷方式。
    配置与日志位于 %LOCALAPPDATA%\Pulse（settings.ini、pulse_net.log）。
    发现这些文件时默认询问是否删除。不需要管理员。
#>
[CmdletBinding()]
param(
    [switch]$RemoveConfig,
    [switch]$KeepConfig
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw $Message
}

function Read-YesNo([string]$Prompt) {
    while ($true) {
        $answer = Read-Host ($Prompt + ' (Y/N)')
        if ($null -eq $answer) {
            Fail '无法读取输入。请在终端中运行，或改用 -RemoveConfig / -KeepConfig。'
        }
        $answer = $answer.Trim()
        if ($answer -match '^(Y|y|是)$') { return $true }
        if ($answer -match '^(N|n|否)$') { return $false }
        Write-Host '请输入 Y 或 N。'
    }
}

function Stop-Pulse {
    $procs = @(Get-Process -Name 'Pulse' -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) {
        Write-Host 'Pulse 未在运行。'
        return
    }
    Write-Host '正在退出正在运行的 Pulse...'
    foreach ($proc in $procs) {
        try { [void]$proc.CloseMainWindow() } catch { }
    }
    $grace = (Get-Date).AddSeconds(5)
    do {
        $left = @(Get-Process -Name 'Pulse' -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) { break }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $grace)

    $left = @(Get-Process -Name 'Pulse' -ErrorAction SilentlyContinue)
    foreach ($proc in $left) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }

    $deadline = (Get-Date).AddSeconds(8)
    do {
        $left = @(Get-Process -Name 'Pulse' -ErrorAction SilentlyContinue)
        if ($left.Count -eq 0) {
            Start-Sleep -Milliseconds 400
            return
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $deadline)
    Fail '无法结束正在运行的 Pulse。请从托盘菜单选择“退出”后重试。'
}

function Remove-PulseAutostart {
    $keyPath = 'Software\Microsoft\Windows\CurrentVersion\Run'
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($keyPath, $true)
    if ($null -eq $key) {
        Write-Host '未找到开机自启注册表项，跳过。'
        return
    }
    try {
        $current = $key.GetValue('Pulse')
        if ($null -eq $current) {
            Write-Host '未设置开机自启，无需移除。'
            return
        }
        $key.DeleteValue('Pulse', $false)
        Write-Host '已移除开机自启（HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Pulse）。'
    } catch {
        Fail ("无法删除开机自启：{0}" -f $_.Exception.Message)
    } finally {
        $key.Close()
    }
}

function Remove-KnownShortcut([string]$ShortcutPath, [string]$Label) {
    if (-not (Test-Path -LiteralPath $ShortcutPath -PathType Leaf)) { return }
    $target = ''
    try {
        $shell = New-Object -ComObject WScript.Shell
        $target = [string]$shell.CreateShortcut($ShortcutPath).TargetPath
    } catch {
        Fail ("无法读取快捷方式 `"{0}`"：{1}" -f $ShortcutPath, $_.Exception.Message)
    }
    $fileName = [System.IO.Path]::GetFileName($target)
    $broken = [string]::IsNullOrWhiteSpace($target)
    if ((-not $broken) -and ($fileName -ne 'Pulse.exe')) {
        Write-Host ("已跳过{0}（目标不是 Pulse.exe）：`"{1}`"" -f $Label, $ShortcutPath)
        return
    }
    Remove-Item -LiteralPath $ShortcutPath -Force
    if ($broken) {
        Write-Host ("已删除失效的{0}：`"{1}`"" -f $Label, $ShortcutPath)
    } else {
        Write-Host ("已删除{0}：`"{1}`"" -f $Label, $ShortcutPath)
    }
}

function Get-PulseDataFiles {
    $result = @()
    $names = @('settings.ini', 'pulse_net.log')
    $dirs = @()
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $dirs += (Join-Path $env:LOCALAPPDATA 'Pulse')
    }
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $dirs += (Join-Path $env:APPDATA 'Pulse')
    }
    foreach ($dir in $dirs) {
        foreach ($name in $names) {
            $path = Join-Path $dir $name
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                $result += [System.IO.Path]::GetFullPath($path)
            }
        }
    }
    return ,$result
}

function Remove-EmptyDirectory([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return }
    $leaf = Split-Path -Leaf $Directory
    if ($leaf -ne 'Pulse') {
        Fail ("拒绝删除名称不是 Pulse 的目录：`"{0}`"" -f $Directory)
    }
    $remaining = @(Get-ChildItem -LiteralPath $Directory -Force -ErrorAction SilentlyContinue)
    if ($remaining.Count -eq 0) {
        Remove-Item -LiteralPath $Directory -Force
        Write-Host ("已删除空目录：`"{0}`"" -f $Directory)
    }
}

try {
    if ($RemoveConfig -and $KeepConfig) {
        Fail '不能同时指定 -RemoveConfig 和 -KeepConfig。'
    }
    if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        Fail '环境变量 LOCALAPPDATA 未设置，无法确定安装目录。'
    }

    $localRoot = [System.IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\')
    $installDir = [System.IO.Path]::GetFullPath((Join-Path $localRoot 'Pulse'))
    $installParent = [System.IO.Path]::GetFullPath((Split-Path -Parent $installDir)).TrimEnd('\')
    if (-not [string]::Equals($installParent, $localRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail ("安装目录不在 LOCALAPPDATA 下：`"{0}`"" -f $installDir)
    }
    if ((Split-Path -Leaf $installDir) -ne 'Pulse') {
        Fail ("安装目录无效：`"{0}`"" -f $installDir)
    }

    Stop-Pulse
    Remove-PulseAutostart

    $programs = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Programs)
    $desktop = [System.Environment]::GetFolderPath([System.Environment+SpecialFolder]::Desktop)
    if (-not [string]::IsNullOrWhiteSpace($programs)) {
        Remove-KnownShortcut -ShortcutPath (Join-Path $programs 'Pulse.lnk') -Label '开始菜单快捷方式'
    } else {
        Write-Host '无法确定开始菜单目录，已跳过该快捷方式。'
    }
    if (-not [string]::IsNullOrWhiteSpace($desktop)) {
        Remove-KnownShortcut -ShortcutPath (Join-Path $desktop 'Pulse.lnk') -Label '桌面快捷方式'
    } else {
        Write-Host '无法确定桌面目录，已跳过该快捷方式。'
    }

    $dataFiles = @(Get-PulseDataFiles)
    $removeData = $false
    if ($RemoveConfig) {
        $removeData = $true
    } elseif ($KeepConfig) {
        $removeData = $false
    } elseif ($dataFiles.Count -gt 0) {
        Write-Host '发现以下配置或日志文件：'
        foreach ($file in $dataFiles) {
            Write-Host ("  `"{0}`"" -f $file)
        }
        Write-Host ("选 Y 会删除整个安装目录 `"{0}`"（含 Pulse.exe 和上述文件）。选 N 只删除 Pulse.exe 和快捷方式，保留这些文件。" -f $installDir)
        $removeData = Read-YesNo '是否一并删除这些文件？'
    } else {
        Write-Host ("未发现 settings.ini 或 pulse_net.log，将删除安装目录 `"{0}`"。" -f $installDir)
        $removeData = $true
    }

    if ($removeData) {
        $installPrefix = $installDir.TrimEnd('\') + '\'
        foreach ($file in $dataFiles) {
            $inInstall = $file.StartsWith($installPrefix, [System.StringComparison]::OrdinalIgnoreCase)
            if (-not $inInstall) {
                Remove-Item -LiteralPath $file -Force
                Write-Host ("已删除：`"{0}`"" -f $file)
            }
        }
        if (Test-Path -LiteralPath $installDir) {
            Remove-Item -LiteralPath $installDir -Recurse -Force
            Write-Host ("已删除安装目录：`"{0}`"" -f $installDir)
        } else {
            Write-Host ("未找到安装目录：`"{0}`"" -f $installDir)
        }
        if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
            $roaming = [System.IO.Path]::GetFullPath((Join-Path $env:APPDATA 'Pulse'))
            Remove-EmptyDirectory -Directory $roaming
        }
    } else {
        $exe = Join-Path $installDir 'Pulse.exe'
        if (Test-Path -LiteralPath $exe -PathType Leaf) {
            $existing = Get-Item -LiteralPath $exe -Force
            if ($existing.IsReadOnly) { $existing.IsReadOnly = $false }
            Remove-Item -LiteralPath $exe -Force
            Write-Host ("已删除程序：`"{0}`"" -f $exe)
        } else {
            Write-Host ("未找到程序：`"{0}`"" -f $exe)
        }
        if ($dataFiles.Count -gt 0) {
            Write-Host '已保留配置和日志。'
        } else {
            Write-Host '没有需要保留的配置或日志。'
        }
        Remove-EmptyDirectory -Directory $installDir
    }

    Write-Host '卸载完成。'
} catch {
    $message = $_.Exception.Message
    if ([string]::IsNullOrWhiteSpace($message)) { $message = [string]$_ }
    Write-Host ("错误：" + $message)
    exit 1
}
exit 0
