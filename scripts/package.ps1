#Requires -Version 5.1
<#
.SYNOPSIS
    编译（可选）并打出 dist\Pulse-<版本>-win-x64.zip。
.DESCRIPTION
    在仓库根目录调用 build.bat。Pulse.exe 已存在且指定 -NoBuild 时跳过编译。
    版本默认来自 app.rc 的 FileVersion，读不到时用 0.1.0。
#>
[CmdletBinding()]
param(
    [string]$Version,
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw $Message
}

function Get-RcFileVersion([string]$RcPath) {
    if (-not (Test-Path -LiteralPath $RcPath -PathType Leaf)) { return '' }
    $text = [System.IO.File]::ReadAllText($RcPath)
    $match = [regex]::Match($text, 'VALUE\s+"FileVersion"\s*,\s*"([^"\\]+)')
    if ($match.Success) { return $match.Groups[1].Value.Trim() }
    $match = [regex]::Match($text, 'FILEVERSION\s+(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)')
    if ($match.Success) {
        return '{0}.{1}.{2}.{3}' -f $match.Groups[1].Value, $match.Groups[2].Value, $match.Groups[3].Value, $match.Groups[4].Value
    }
    return ''
}

function Invoke-PulseBuild([string]$BuildBat, [string]$Root) {
    if ([string]::IsNullOrWhiteSpace($env:ComSpec)) {
        Fail '找不到 cmd.exe（环境变量 ComSpec 未设置）。'
    }
    if ($BuildBat.Contains('"')) {
        Fail ("build.bat 路径不能包含双引号：`"{0}`"" -f $BuildBat)
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $env:ComSpec
    $psi.Arguments = '/d /c call "' + $BuildBat + '"'
    $psi.WorkingDirectory = $Root
    $psi.UseShellExecute = $false
    Write-Host ("正在编译：`"{0}`"" -f $BuildBat)
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) {
        Fail '无法启动 build.bat。'
    }
    $proc.WaitForExit()
    if ($proc.ExitCode -ne 0) {
        Fail ("编译失败，build.bat 退出码为 {0}。请确认 MinGW-w64 g++ 13.2 和 windres 已加入 PATH。" -f $proc.ExitCode)
    }
}

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        Fail ("缺少文件：`"{0}`"" -f $Source)
    }
    $parent = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

$script:stage = $null
$script:failed = $false
$script:failMessage = ''
try {
    if ([string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        Fail '无法确定脚本所在目录。请用 -File 运行此脚本。'
    }
    $root = Split-Path -Parent $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($root)) {
        Fail '无法确定仓库根目录。'
    }
    $root = [System.IO.Path]::GetFullPath($root)
    $build = Join-Path $root 'build.bat'
    $exe = Join-Path $root 'Pulse.exe'
    if (-not (Test-Path -LiteralPath $build -PathType Leaf)) {
        Fail ("未在脚本上一级找到 build.bat：`"{0}`"" -f $build)
    }

    if ($NoBuild) {
        if (Test-Path -LiteralPath $exe -PathType Leaf) {
            Write-Host ("已存在 Pulse.exe，跳过编译（-NoBuild）：`"{0}`"" -f $exe)
        } else {
            Fail ("指定了 -NoBuild，但找不到 Pulse.exe：`"{0}`"" -f $exe)
        }
    } else {
        Invoke-PulseBuild -BuildBat $build -Root $root
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            Fail ("编译结束但未生成 Pulse.exe：`"{0}`"" -f $exe)
        }
    }

    $version = $Version
    if ([string]::IsNullOrWhiteSpace($version)) {
        $version = Get-RcFileVersion -RcPath (Join-Path $root 'app.rc')
        if ([string]::IsNullOrWhiteSpace($version)) {
            $version = '0.1.0'
            Write-Host '未能从 app.rc 读取 FileVersion，使用 0.1.0。'
        } else {
            Write-Host ("版本来自 app.rc FileVersion：{0}" -f $version)
        }
    }
    $version = $version.Trim()
    if ($version -notmatch '^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$') {
        Fail ("版本号无效：{0}。只允许字母、数字、点、下划线和连字符。" -f $version)
    }

    $dist = Join-Path $root 'dist'
    if (-not (Test-Path -LiteralPath $dist)) {
        New-Item -ItemType Directory -Path $dist -Force | Out-Null
    }
    $zipPath = Join-Path $dist ("Pulse-{0}-win-x64.zip" -f $version)
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    $script:stage = Join-Path ([System.IO.Path]::GetTempPath()) ('Pulse-stage-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $script:stage -Force | Out-Null
    $stageScripts = Join-Path $script:stage 'scripts'
    New-Item -ItemType Directory -Path $stageScripts -Force | Out-Null

    Copy-Required -Source $exe -Destination (Join-Path $script:stage 'Pulse.exe')
    Copy-Required -Source (Join-Path $root 'README.md') -Destination (Join-Path $script:stage 'README.md')
    Copy-Required -Source (Join-Path $root 'LICENSE') -Destination (Join-Path $script:stage 'LICENSE')
    Copy-Required -Source (Join-Path $root 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $script:stage 'THIRD_PARTY_NOTICES.md')
    foreach ($name in @('install.ps1', 'install.cmd', 'uninstall.ps1', 'uninstall.cmd')) {
        Copy-Required -Source (Join-Path $PSScriptRoot $name) -Destination (Join-Path $stageScripts $name)
    }

    $paths = @(
        (Join-Path $script:stage 'Pulse.exe'),
        (Join-Path $script:stage 'README.md'),
        (Join-Path $script:stage 'LICENSE'),
        (Join-Path $script:stage 'THIRD_PARTY_NOTICES.md'),
        $stageScripts
    )
    $oldProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Compress-Archive -Path $paths -DestinationPath $zipPath -CompressionLevel Optimal
    } catch {
        Fail ("打包失败：{0}" -f $_.Exception.Message)
    } finally {
        $ProgressPreference = $oldProgress
    }

    if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
        Fail ("未生成压缩包：`"{0}`"" -f $zipPath)
    }
    $info = Get-Item -LiteralPath $zipPath
    Write-Host ("已生成：`"{0}`"" -f $info.FullName)
    Write-Host ("大小：{0} 字节" -f $info.Length)
} catch {
    $script:failed = $true
    $script:failMessage = $_.Exception.Message
    if ([string]::IsNullOrWhiteSpace($script:failMessage)) { $script:failMessage = [string]$_ }
} finally {
    if (-not [string]::IsNullOrWhiteSpace($script:stage) -and (Test-Path -LiteralPath $script:stage)) {
        Remove-Item -LiteralPath $script:stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if ($script:failed) {
    Write-Host ("错误：" + $script:failMessage)
    exit 1
}
exit 0
