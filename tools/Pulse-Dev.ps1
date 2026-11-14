[CmdletBinding()]
param(
    [ValidateSet('Build','Release','Test','Test-Continue','Debug','Run','Trial','Trial-Test','Capacity','Artifact-Status','Artifact-Review','Artifact-Retire','Merge-Upstream','Script-Test','Workflow-Test','Release-Source','Release-Import','Release-Package','Release-Prepare','Release-Status','Release-Publish','Release-Verify','Reconcile-Wait','Reconcile-Capacity')][string]$Action = 'Test',
    [ValidateSet('daily','idle')][string]$Profile = 'daily',
    [switch]$Interactive,
    [string]$ArtifactExe,
    [string]$ReleaseInput,
    [string]$RecoveryInput
)
$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Console]::OutputEncoding = $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
$configPath = Join-Path $PSScriptRoot 'dev-environment.local.json'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if (-not (Test-Path -LiteralPath $config.node -PathType Leaf)) { throw 'Configured Node runtime is missing.' }
$runnerArguments = @('--max-old-space-size=64', (Join-Path $PSScriptRoot 'pulse_dev.cjs'), $Action.ToLowerInvariant(), $Profile)
if ($Interactive) { $runnerArguments += '--interactive' }
elseif ($Action -eq 'Trial-Test') { $runnerArguments += '--isolated' }
if ($ArtifactExe) { $runnerArguments += $ArtifactExe }
if ($PSBoundParameters.ContainsKey('RecoveryInput')) {
    if ($Action -notin @('Reconcile-Capacity','Test-Continue','Artifact-Retire') -or $PSBoundParameters.ContainsKey('ReleaseInput')) { throw 'RecoveryInput only applies to reconciliation, continuation or exact retirement.' }
    $RecoveryInput | & $config.node @runnerArguments
} elseif ($Action -in @('Reconcile-Capacity','Test-Continue','Artifact-Retire')) {
    throw 'Reconciliation/continuation requires exact RecoveryInput.'
} elseif ($PSBoundParameters.ContainsKey('ReleaseInput')) {
    if ($Action -notin @('Release-Source','Release-Import','Release-Package','Release-Prepare','Release-Status','Release-Publish','Release-Verify')) { throw 'ReleaseInput only applies to release stages.' }
    $ReleaseInput | & $config.node @runnerArguments
} else {
    & $config.node @runnerArguments
}
if ($LASTEXITCODE -ne 0) { throw "Pulse development entry failed with exit code $LASTEXITCODE; inspect its retained receipt." }
