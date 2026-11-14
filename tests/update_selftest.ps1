[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$FixtureRoot)
$ErrorActionPreference='Stop'
[Console]::InputEncoding=[Console]::OutputEncoding=$OutputEncoding=[Text.UTF8Encoding]::new($false)
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
$root=Split-Path -Parent $PSScriptRoot
# Decode the embedded UTF-8 script exactly as the production BOM extraction does.
. ([ScriptBlock]::Create([IO.File]::ReadAllText((Join-Path $root 'src/update.ps1'),[Text.Encoding]::UTF8))) -LibraryOnly
Add-Type -AssemblyName System.IO.Compression.FileSystem,System.Net.Http
Require (-not (Test-Path -LiteralPath $FixtureRoot)) 'Fixture must be new.'
$null=[IO.Directory]::CreateDirectory($FixtureRoot)
$script:Form=$null
function Reject([scriptblock]$call,[string]$name) {
    $rejected=$false; try{& $call}catch{$rejected=$true}; Require $rejected ('Expected rejection: '+$name)
}
Require ((Version 'custom-2026.11.10.10') -gt (Version 'custom-2026.11.10.2')) 'Numeric revision comparison.'
Require ((Version 'custom-2026.11.10.2') -gt (Version 'custom-2026.10.23.1')) 'Date version comparison.'
Require ((Version 'custom-2026.11.10') -eq [Version]'2026.11.10.0') 'Legacy three-component tag.'
Reject {Version 'custom-2026.02.31.1'} 'Invalid date'
Reject {Version 'v1.3.0'} 'Different release channel'
Reject {Version 'custom-2026.11.10.1-beta'} 'Prerelease'
Reject {AllowedUrl 'http://github.com/test'} 'HTTP'
Reject {AllowedUrl 'https://github.com.evil.test/test'} 'Host spoof'
Reject {AllowedUrl 'https://user:pass@github.com/test'} 'Credentials'
Reject {AllowedUrl 'https://github.com:444/test'} 'Unexpected port'
$null=AllowedUrl 'https://release-assets.githubusercontent.com/github-production-release-asset/test?sig=test'
# Even an already-completed read must obey the whole-download deadline.
$completion=[Threading.Tasks.TaskCompletionSource[int]]::new()
$completion.SetResult(7); $completed=$completion.Task
$expired=[pscustomobject]@{Elapsed=[TimeSpan]::FromSeconds(91)}
Reject {WaitTask $completed $expired} 'Completed read after total deadline'
Require ((WaitTask $completed ([Diagnostics.Stopwatch]::StartNew())) -eq 7) 'Completed read within deadline.'
# Exercise the real streaming/redirect client without issuing any network request.
Add-Type -ReferencedAssemblies ([Net.Http.HttpMessageHandler].Assembly.Location) -TypeDefinition @'
using System;
using System.Net;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
public class PulseFakeHttp : HttpMessageHandler {
 public int Mode;
 protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken token) {
  var r = new HttpResponseMessage(Mode == 1 ? HttpStatusCode.Redirect : Mode == 2 ? HttpStatusCode.Forbidden : HttpStatusCode.OK);
  r.Content = new ByteArrayContent(new byte[]{0,128,255});
  if(Mode == 1) r.Headers.Location = new Uri("https://evil.test/payload");
  return Task.FromResult(r);
 }
}
'@
$raw=Download 'https://api.github.com/test' 3 '' ([PulseFakeHttp]::new())
Require ($raw.Length -eq 3 -and $raw[0] -eq 0 -and $raw[1] -eq 128 -and $raw[2] -eq 255) 'Binary stream preserved.'
$binary=Join-Path $FixtureRoot 'download.bin'
$script:Progress=[pscustomobject]@{Style='Marquee';Value=0}
$label=[pscustomobject]@{Text=''}
Download 'https://api.github.com/test' 3 $binary ([PulseFakeHttp]::new())
Require ($script:Progress.Style -eq 'Continuous' -and $script:Progress.Value -eq 100 -and $label.Text -match 'KB') 'Download progress reaches 100 percent with received bytes.'
$script:Progress=$null
$script:Form=[pscustomobject]@{IsDisposed=$true};$script:Cancelled=$true
Reject {PulseUi} 'Cancellation after closing the progress window'
$script:Form=$null;$script:Cancelled=$false
Require ([BitConverter]::ToString([IO.File]::ReadAllBytes($binary)) -eq '00-80-FF') 'Binary file download.'
Reject {Download 'https://api.github.com/test' 2 '' ([PulseFakeHttp]::new())} 'Oversized HTTP body'
Reject {Download 'https://api.github.com/test' 3 $binary ([PulseFakeHttp]::new())} 'Never overwrite download'
Reject {$h=[PulseFakeHttp]::new(); $h.Mode=1; Download 'https://api.github.com/test' 3 '' $h} 'Untrusted redirect'
Reject {$h=[PulseFakeHttp]::new(); $h.Mode=2; Download 'https://api.github.com/test' 3 '' $h} 'HTTP error'
$release=[pscustomobject]@{tag_name='custom-2026.11.10.3'; assets=@([pscustomobject]@{name='release-manifest.json';state='uploaded';browser_download_url='https://github.com/haihaihailailo/Pulse/releases/download/custom-2026.11.10.3/release-manifest.json'})}
$null=AssetUrl $release 'release-manifest.json'
$release.assets[0].browser_download_url='https://github.com/other/repo/releases/download/test/release-manifest.json'
Reject {AssetUrl $release 'release-manifest.json'} 'Different repository'
$old=Join-Path $FixtureRoot 'Pulse old 中文.exe'; $candidate=Join-Path $FixtureRoot 'Pulse candidate.exe'
$backup=Join-Path $FixtureRoot 'rollback.exe'; $failed=Join-Path $FixtureRoot 'failed.exe'
$settings=Join-Path $FixtureRoot 'settings.ini'
[IO.File]::WriteAllText($old,'original'); [IO.File]::WriteAllText($candidate,'candidate')
[IO.File]::WriteAllText($settings,'user setting = 保留',[Text.Encoding]::UTF8)
$settingsHash=Hash $settings; $oldHash=Hash $old; $newHash=Hash $candidate
Reject {ReplaceAndRestart $old $candidate $backup $failed ('0'*64) $newHash {throw 'Must not launch'}} 'Old identity changed'
Require ((Hash $old) -eq $oldHash -and (Hash $candidate) -eq $newHash) 'Preflight preserves files.'
Reject {CheckHash $candidate ('f'*64)} 'Corrupt download'
$script:started=@()
ReplaceAndRestart $old $candidate $backup $failed $oldHash $newHash {param($file) $script:started+=Hash $file}
Require ((Hash $old) -eq $newHash -and (Hash $backup) -eq $oldHash -and $script:started.Count -eq 1) 'Successful atomic replacement and restart.'
$candidate2=Join-Path $FixtureRoot 'candidate2.exe'; [IO.File]::WriteAllText($candidate2,'broken candidate')
$brokenHash=Hash $candidate2; $backup2=Join-Path $FixtureRoot 'rollback2.exe'; $failed2=Join-Path $FixtureRoot 'failed2.exe'
$script:started=@()
Reject {ReplaceAndRestart $old $candidate2 $backup2 $failed2 $newHash $brokenHash {param($file)
    $h=Hash $file; $script:started+=$h; if($h -eq $brokenHash){throw 'Simulated startup failure'}
}} 'Startup failure'
Require ((Hash $old) -eq $newHash -and (Hash $failed2) -eq $brokenHash -and $script:started.Count -eq 2 -and $script:started[1] -eq $newHash) 'Rollback restores and restarts the old executable.'
Require ((Hash $settings) -eq $settingsHash) 'User settings preserved.'
# Real ZIP extraction, including malicious path entries that must never be extracted.
$zip=Join-Path $FixtureRoot 'test.zip'; $archive=[IO.Compression.ZipFile]::Open($zip,'Create')
try {
    foreach($name in @('Pulse.exe','../escape.exe')){
        $entry=$archive.CreateEntry($name); $writer=[IO.StreamWriter]::new($entry.Open()); try{$writer.Write('candidate')}finally{$writer.Dispose()}
    }
}finally{$archive.Dispose()}
$manifest=[pscustomobject]@{executable=[pscustomobject]@{bytes=9;sha256=$newHash}}
$extracted=Join-Path $FixtureRoot 'extracted.exe'; ExtractExe $zip $extracted $manifest
Require ((Hash $extracted) -eq $newHash -and -not (Test-Path -LiteralPath (Join-Path (Split-Path $FixtureRoot) 'escape.exe'))) 'Only the verified root executable is extracted.'
$manifest.executable.bytes=33554433
Reject {ExtractExe $zip (Join-Path $FixtureRoot 'oversize.exe') $manifest} 'Expansion size mismatch'
Write-Output 'update_selftest: PASS (version, HTTPS origin, hash refusal, atomic replace, rollback/restart, settings, ZIP confinement)'
