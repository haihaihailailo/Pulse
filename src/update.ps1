# Embedded in Pulse.exe; Windows PowerShell 5.1. Never changes user settings.
[CmdletBinding()]
param([string]$Target, [string]$CurrentVersion, [int]$OwnerPid,
      [long]$OwnerCreated, [long]$OwnerWindow,
      [ValidateSet('zh','en')][string]$Language='zh', [switch]$LibraryOnly)
$ErrorActionPreference='Stop'
[Console]::InputEncoding=[Console]::OutputEncoding=$OutputEncoding=[Text.UTF8Encoding]::new($false)
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
$Repository='haihaihailailo/Pulse'
function Text([string]$zh,[string]$en) { if($Language -eq 'en'){$en}else{$zh} }
function Require($condition,[string]$message) { if(-not $condition){throw $message} }
function Version([string]$value) {
    Require ($value -cmatch '^custom-(20\d{2})\.(\d{2})\.(\d{2})(?:\.(\d{1,4}))?$') 'Unsupported release version.'
    $year=[int]$Matches[1]; $month=[int]$Matches[2]; $day=[int]$Matches[3]
    $revision=0; if($Matches[4]){$revision=[int]$Matches[4]}
    $null=[DateTime]::new($year,$month,$day)
    return [Version]::new($year,$month,$day,$revision)
}
function Regular([string]$path) {
    Require ([IO.Path]::IsPathRooted($path)) 'Absolute path required.'
    $item=Get-Item -LiteralPath $path -Force
    Require (-not $item.PSIsContainer) 'Regular file required.'
    while($item) {
        Require (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Linked paths are not supported.'
        if($item.PSIsContainer){$item=$item.Parent}else{$item=$item.Directory}
    }
}
function Hash([string]$path) {
    Regular $path
    $stream=[IO.File]::OpenRead($path); $algorithm=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}
    finally {$algorithm.Dispose(); $stream.Dispose()}
}
function CheckHash([string]$path,[string]$expected) {
    Require ($expected -cmatch '^[0-9a-f]{64}$') 'Invalid SHA256.'
    Require ((Hash $path) -ceq $expected) 'SHA256 mismatch; update refused.'
}
function AllowedUrl([string]$url) {
    $uri=[Uri]$url
    Require ($uri.Scheme -eq 'https' -and $uri.IsDefaultPort -and -not $uri.UserInfo -and -not $uri.Fragment -and
        $uri.DnsSafeHost -in @('api.github.com','github.com','release-assets.githubusercontent.com','objects.githubusercontent.com')) 'Untrusted download address.'
    return $uri
}
function PulseUi {
    if($script:Form) {
        [Windows.Forms.Application]::DoEvents()
        if($script:Cancelled){throw 'Cancelled; the existing application was not replaced.'}
    }
}
function WaitTask($task,$watch) {
    while(-not $task.IsCompleted) {
        PulseUi
        if($watch.Elapsed.TotalSeconds -gt 90){throw 'Download timed out.'}
        Start-Sleep -Milliseconds 50
    }
    $task.GetAwaiter().GetResult()
}
# Redirects are explicitly bounded and constrained to GitHub's asset hosts.
function Download([string]$url,[long]$limit,[string]$destination='', $handler=$null) {
    if(-not $handler) {
        $handler=[Net.Http.HttpClientHandler]::new(); $handler.AllowAutoRedirect=$false; $handler.UseCookies=$false
        $handler.UseDefaultCredentials=$false
    }
    $client=[Net.Http.HttpClient]::new($handler); $client.Timeout=[TimeSpan]::FromSeconds(90)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('Pulse-Updater/1.0')
    $response=$null; $inputStream=$null; $output=$null; $watch=[Diagnostics.Stopwatch]::StartNew()
    try {
        for($redirect=0;$redirect -le 5;$redirect++) {
            $uri=AllowedUrl $url
            $response=WaitTask ($client.GetAsync($uri,[Net.Http.HttpCompletionOption]::ResponseHeadersRead)) $watch
            if([int]$response.StatusCode -in @(301,302,303,307,308)) {
                Require ($redirect -lt 5 -and $response.Headers.Location) 'Too many redirects.'
                $url=[Uri]::new($uri,$response.Headers.Location).AbsoluteUri
                $response.Dispose(); $response=$null; continue
            }
            Require ($response.IsSuccessStatusCode) ('GitHub returned HTTP '+[int]$response.StatusCode+'.')
            break
        }
        Require ($null -eq $response.Content.Headers.ContentLength -or $response.Content.Headers.ContentLength -le $limit) 'Download exceeds size limit.'
        $inputStream=WaitTask ($response.Content.ReadAsStreamAsync()) $watch
        if($destination){$output=[IO.File]::Open($destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)}
        else{$output=[IO.MemoryStream]::new()}
        $buffer=New-Object byte[] 32768; $total=0L
        while($true) {
            $count=WaitTask ($inputStream.ReadAsync($buffer,0,$buffer.Length)) $watch
            if(-not $count){break}
            $total+=$count; Require ($total -le $limit) 'Download exceeds size limit.'
            $output.Write($buffer,0,$count); PulseUi
        }
        if(-not $destination){return ,[byte[]]$output.ToArray()}
    } finally {
        if($inputStream){$inputStream.Dispose()}; if($output){$output.Dispose()}
        if($response){$response.Dispose()}; $client.Dispose(); $handler.Dispose()
    }
}
function JsonDownload([string]$url) { [Text.Encoding]::UTF8.GetString((Download $url 1048576)) | ConvertFrom-Json }
function AssetUrl($release,[string]$name) {
    $assets=@($release.assets | Where-Object { $_.name -ceq $name -and $_.state -eq 'uploaded' })
    Require ($assets.Count -eq 1) ('Missing or duplicate release asset: '+$name)
    $expected='https://github.com/'+$Repository+'/releases/download/'+$release.tag_name+'/'+$name
    Require ($assets[0].browser_download_url -ceq $expected) 'Release asset origin mismatch.'
    return $expected
}
function ExtractExe([string]$zip,[string]$output,$manifest) {
    $archive=[IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entries=@($archive.Entries | Where-Object { $_.FullName -ceq 'Pulse.exe' })
        Require ($entries.Count -eq 1 -and $entries[0].Length -gt 0 -and $entries[0].Length -le 33554432 -and
            $entries[0].Length -eq [long]$manifest.executable.bytes) 'Invalid executable in archive.'
        # Extract this single root entry; never extract paths supplied by a ZIP.
        $source=$entries[0].Open(); $dest=$null
        try {
            $dest=[IO.File]::Open($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            $buffer=New-Object byte[] 32768; $total=0L
            while(($n=$source.Read($buffer,0,$buffer.Length)) -gt 0){$total+=$n; Require ($total -le 33554432) 'Expanded size limit.'; $dest.Write($buffer,0,$n)}
        } finally {if($dest){$dest.Dispose()}; $source.Dispose()}
    } finally {$archive.Dispose()}
    CheckHash $output $manifest.executable.sha256
    Require ((Get-Item -LiteralPath $output).Length -eq [long]$manifest.executable.bytes) 'Executable size mismatch.'
}
# Shared transaction is exercised on synthetic files by the protected test runner.
function ReplaceAndRestart([string]$targetFile,[string]$candidate,[string]$backup,[string]$failed,
                          [string]$oldHash,[string]$newHash,[scriptblock]$start) {
    CheckHash $targetFile $oldHash; CheckHash $candidate $newHash
    Require (-not (Test-Path -LiteralPath $backup) -and -not (Test-Path -LiteralPath $failed)) 'Rollback path already exists.'
    $exchanged=$false
    try {
        [IO.File]::Replace($candidate,$targetFile,$backup,$false); $exchanged=$true
        CheckHash $targetFile $newHash; & $start $targetFile
    }
    catch {
        $failure=$_
        if($exchanged) {
            CheckHash $targetFile $newHash; CheckHash $backup $oldHash
            [IO.File]::Replace($backup,$targetFile,$failed,$false)
        } else {CheckHash $targetFile $oldHash}
        try { & $start $targetFile } catch { throw ('Old executable restored, but restart failed. Start Pulse manually. '+$failure.Exception.Message) }
        throw ('Update failed; old executable restored and restarted. '+$failure.Exception.Message)
    }
}
if($LibraryOnly){return}

$script:Form=$null; $script:Cancelled=$false; $mutex=$null; $ownsMutex=$false
$staged=$null; $zip=$null; $closed=$false; $replaced=$false; $oldHash=$null
try {
    Add-Type -AssemblyName System.Windows.Forms,System.Drawing,System.Net.Http,System.IO.Compression.FileSystem
    Regular $Target; $Target=[IO.Path]::GetFullPath($Target); $oldHash=Hash $Target
    $current=Version $CurrentVersion
    $mutex=[Threading.Mutex]::new($false,'Local\Pulse.Update.'+$oldHash)
    try{$ownsMutex=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$ownsMutex=$true}
    Require $ownsMutex (Text '已有更新窗口正在运行。' 'An update window is already running.')
    $owner=[Diagnostics.Process]::GetProcessById($OwnerPid)
    Require ($owner.StartTime.ToUniversalTime().ToFileTimeUtc() -eq $OwnerCreated -and $owner.MainModule.FileName -ieq $Target) 'Running application identity changed.'
    $script:Form=[Windows.Forms.Form]::new(); $script:Form.Text='Pulse '+(Text '更新' 'Update')
    $script:Form.Size=[Drawing.Size]::new(480,155); $script:Form.StartPosition='CenterScreen'
    $script:Form.FormBorderStyle='FixedDialog'; $script:Form.MaximizeBox=$false
    $label=[Windows.Forms.Label]::new(); $label.AutoSize=$false; $label.Dock='Fill'; $label.Padding=[Windows.Forms.Padding]::new(20)
    $label.Text=(Text '正在检查更新…' 'Checking for updates…')+"`r`n"+$CurrentVersion
    $script:Form.Controls.Add($label); $script:Form.Add_FormClosing({$script:Cancelled=$true}); $script:Form.Show(); PulseUi
    [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
    $release=JsonDownload ('https://api.github.com/repos/'+$Repository+'/releases/latest')
    Require (-not $release.draft -and -not $release.prerelease) 'Only stable releases are accepted.'
    $latest=Version $release.tag_name
    if($latest -le $current) {
        $script:Form.Hide()
        $null=[Windows.Forms.MessageBox]::Show((Text '当前已是最新版本：' 'You are up to date: ')+$CurrentVersion,'Pulse')
        return
    }
    $script:Form.Hide()
    $prompt=(Text '发现新版：' 'New version: ')+$release.tag_name+"`r`n"+(Text '下载并替换当前程序，然后自动重启？现有设置将保留。' 'Download, replace this application and restart? Your settings will be preserved.')
    if([Windows.Forms.MessageBox]::Show($prompt,'Pulse','YesNo','Question') -ne 'Yes'){return}
    $script:Form.Show(); $label.Text=Text '正在下载并校验更新… 可关闭此窗口取消。' 'Downloading and verifying… Close this window to cancel.'; PulseUi
    $manifest=JsonDownload (AssetUrl $release 'release-manifest.json')
    Require ($manifest.repository -ceq $Repository -and $manifest.tag -ceq $release.tag_name -and
        $manifest.architecture -ceq 'Windows x64' -and $manifest.executable.name -ceq 'Pulse.exe' -and
        $manifest.commit -cmatch '^[0-9a-f]{40}$') 'Release manifest mismatch.'
    $zipName='Pulse-'+$release.tag_name+'-win-x64.zip'
    $sums=[Text.Encoding]::UTF8.GetString((Download (AssetUrl $release 'SHA256SUMS.txt') 65536))
    $matching=@($sums -split '\r?\n' | Where-Object {$_ -cmatch ('^[0-9a-f]{64}  '+[regex]::Escape($zipName)+'$')})
    Require ($matching.Count -eq 1) 'Missing or duplicate ZIP checksum.'
    $zip=Join-Path $PSScriptRoot 'package.zip'
    Download (AssetUrl $release $zipName) 33554432 $zip
    CheckHash $zip $matching[0].Substring(0,64)
    $id=[Guid]::NewGuid().ToString('N'); $directory=[IO.Path]::GetDirectoryName($Target)
    # Never silently accumulate unlimited rollback executables or delete unowned backups.
    $backupBytes=0L
    foreach($saved in [IO.Directory]::EnumerateFiles($directory,[IO.Path]::GetFileName($Target)+'.rollback-*')) {
        Regular $saved; $backupBytes+=(Get-Item -LiteralPath $saved).Length
    }
    Require ($backupBytes+(Get-Item -LiteralPath $Target).Length -le 134217728) 'Rollback backups reached 128 MiB. Archive old rollback files before updating.'
    # Same-directory staging both checks write access before closing and permits atomic replacement.
    $staged=$Target+'.update-'+$id
    ExtractExe $zip $staged $manifest
    Require ([Diagnostics.FileVersionInfo]::GetVersionInfo($staged).FileVersion -eq $manifest.fileVersion) 'Resource version mismatch.'
    PulseUi
    $owner.Refresh()
    Require (-not $owner.HasExited -and $owner.StartTime.ToUniversalTime().ToFileTimeUtc() -eq $OwnerCreated) 'Original process has exited; update cancelled.'
    CheckHash $Target $oldHash
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PulseUpdateWindow {
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
    $window=[IntPtr]::new($OwnerWindow); [uint32]$windowPid=0
    $null=[PulseUpdateWindow]::GetWindowThreadProcessId($window,[ref]$windowPid)
    Require ($windowPid -eq $OwnerPid) 'Original window identity changed.'
    $label.Text=Text '正在正常退出旧版本并替换…' 'Closing the old version and replacing it…'
    $script:Form.ControlBox=$false; $script:Form.Refresh()
    Require ([PulseUpdateWindow]::PostMessage($window,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)) 'Could not request a normal exit.'
    Require ($owner.WaitForExit(30000)) 'Pulse did not exit; no executable was replaced.'
    $closed=$true
    $backup=$Target+'.rollback-'+$id; $failed=$Target+'.failed-'+$id
    $start={param($file)
        $info=[Diagnostics.ProcessStartInfo]::new($file); $info.WorkingDirectory=[IO.Path]::GetDirectoryName($file); $info.UseShellExecute=$true
        $new=[Diagnostics.Process]::Start($info)
        Require ($null -ne $new) 'Could not restart Pulse.'
        try {Require (-not $new.WaitForExit(3000)) 'Pulse exited during startup.'} finally {$new.Dispose()}
    }
    ReplaceAndRestart $Target $staged $backup $failed $oldHash $manifest.executable.sha256 $start
    $replaced=$true
    $script:Form.Hide()
    $null=[Windows.Forms.MessageBox]::Show((Text '更新完成，已重启。旧程序备份：' 'Updated and restarted. Previous executable: ')+"`r`n"+$backup,'Pulse')
} catch {
    if($script:Form){$script:Form.Hide()}
    $message=(Text '更新未完成：' 'Update did not complete: ')+$_.Exception.Message
    if('Windows.Forms.MessageBox' -as [type]){$null=[Windows.Forms.MessageBox]::Show($message,'Pulse','OK','Warning')}
} finally {
    if($script:Form){$script:Form.Dispose()}
    if($ownsMutex){$mutex.ReleaseMutex()}; if($mutex){$mutex.Dispose()}
    # Only this attempt's exact scratch files. Rollback/failed executables are retained.
    foreach($file in @($staged,$zip)) {
        if($file -and (Test-Path -LiteralPath $file)){try{Regular $file; Remove-Item -LiteralPath $file -Force}catch{}}
    }
    if([IO.Path]::GetFileName($PSScriptRoot) -match '^Pulse\.Update-\{[0-9a-f-]{36}\}$') {
        try {Remove-Item -LiteralPath $PSCommandPath -Force; [IO.Directory]::Delete($PSScriptRoot,$false)} catch {}
    }
}
