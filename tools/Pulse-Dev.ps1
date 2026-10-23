[CmdletBinding()]
param(
    [ValidateSet('Build','Test','Debug','Run','Trial','Trial-Test')][string]$Action = 'Test',
    [ValidateSet('daily','idle')][string]$Profile = 'daily',
    [switch]$Interactive,
    [string]$ArtifactExe
)
$ErrorActionPreference = 'Stop'
$configPath = Join-Path $PSScriptRoot 'dev-environment.local.json'
$config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if (-not (Test-Path -LiteralPath $config.node -PathType Leaf)) { throw 'Configured Node runtime is missing.' }
$runnerArguments = @('--max-old-space-size=64', (Join-Path $PSScriptRoot 'pulse_dev.cjs'), $Action.ToLowerInvariant(), $Profile)
if ($Interactive) { $runnerArguments += '--interactive' }
elseif ($Action -eq 'Trial-Test') { $runnerArguments += '--isolated' }
if ($ArtifactExe) { $runnerArguments += $ArtifactExe }
& $config.node @runnerArguments
if ($LASTEXITCODE -ne 0) { throw "Pulse development entry failed with exit code $LASTEXITCODE; inspect its retained receipt." }
