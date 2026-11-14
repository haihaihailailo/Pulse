[CmdletBinding()]
param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference='Stop'
[Console]::InputEncoding = [Console]::OutputEncoding = $OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$env:PYTHONUTF8='1'; $env:PYTHONIOENCODING='utf-8'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 with Roslyn is required for the settings module.' }
$project=Split-Path -Parent $PSScriptRoot
$target=[IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $target) { throw 'Settings module output already exists; no overwrite.' }
$framework=Join-Path $env:SystemRoot 'Microsoft.NET/Framework64/v4.0.30319'
$references=[Microsoft.CodeAnalysis.MetadataReference[]]@(
    foreach ($name in @('mscorlib','System','System.Core','System.Drawing','System.Windows.Forms')) {
        [Microsoft.CodeAnalysis.MetadataReference]::CreateFromFile((Join-Path $framework ($name+'.dll')),[Microsoft.CodeAnalysis.MetadataReferenceProperties]::Assembly,$null)
    }
)
$trees=[Microsoft.CodeAnalysis.SyntaxTree[]]@(
    foreach ($name in @('LiteUI.cs','PulseSettings.cs')) {
        $text=[IO.File]::ReadAllText((Join-Path $project ('src/settings_ui/'+$name)),[Text.Encoding]::UTF8)
        [Microsoft.CodeAnalysis.CSharp.CSharpSyntaxTree]::ParseText($text,$null,('src/settings_ui/'+$name),[Text.Encoding]::UTF8,[Threading.CancellationToken]::None)
    }
)
$options=[Microsoft.CodeAnalysis.CSharp.CSharpCompilationOptions]::new([Microsoft.CodeAnalysis.OutputKind]::WindowsApplication)
$options=$options.WithOptimizationLevel([Microsoft.CodeAnalysis.OptimizationLevel]::Release).WithPlatform([Microsoft.CodeAnalysis.Platform]::X64).WithDeterministic($true)
$compilation=[Microsoft.CodeAnalysis.CSharp.CSharpCompilation]::Create('Pulse.Settings',$trees,$references,$options)
$manifest=[IO.File]::OpenRead((Join-Path $project 'app.manifest'))
$resources=$null; $outputStream=$null
try {
    $resources=$compilation.CreateDefaultWin32Resources($true,$false,$manifest,$null)
    $outputStream=[IO.File]::Open($target,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $result=$compilation.Emit($outputStream,$null,$null,$resources,$null,$null,[Threading.CancellationToken]::None)
    if (-not $result.Success) { throw (($result.Diagnostics | Where-Object Severity -eq Error | ForEach-Object ToString) -join "`n") }
    Write-Output 'SETTINGS_UI_BUILD PASS (.NET Framework WinForms; embedded; no SDK installation)'
} finally {
    if ($outputStream) { $outputStream.Dispose() }; if ($resources) { $resources.Dispose() }; $manifest.Dispose()
}
