param(
    [Parameter(Mandatory=$true)][string]$BinaryDirectory,
    [Parameter(Mandatory=$true)][string]$BundleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$InnoCompiler
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (!$output.StartsWith($workspace + '\.local\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Setup output must be inside workspace .local.' }
if (Test-Path -LiteralPath $output) { throw 'Choose a new output directory.' }
if ((Get-FileHash -LiteralPath $InnoCompiler -Algorithm SHA256).Hash -ne 'E0F9F1DC32032650C6629F6DEE41EB6D20BA140E4E3C07C25D8A24F9BA781644') { throw 'Pinned Inno Setup 7.1.1 compiler hash mismatch.' }
if ((& $InnoCompiler --version).Trim() -ne '7.1.1') { throw 'Inno Setup 7.1.1 required.' }
$manifest = Get-Content -LiteralPath (Join-Path $BundleDirectory 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.product_id -ne 'MCWiiU-Launcher' -or $manifest.package_format -ne 'zip' -or $manifest.schema_version -ne 2) { throw 'Invalid initial package identity.' }
foreach ($name in @('manifest.sig','foundation.json','foundation.sig',$manifest.package_filename)) { if (!(Test-Path -LiteralPath (Join-Path $BundleDirectory $name) -PathType Leaf)) { throw ('Missing signed setup input: '+$name) } }
$foundation = Get-Content -LiteralPath (Join-Path $BundleDirectory 'foundation.json') -Raw | ConvertFrom-Json
if ($foundation.product_id -ne 'MCWiiU-Launcher' -or $foundation.app_id -ne '{362BA210-0BBE-45ED-A0F5-6000DB57594B}') { throw 'Invalid foundation identity.' }
foreach ($file in $foundation.files) {
    if ($file.path -notin @('MCWiiU-Launcher.exe','MCWiiU-Updater.exe')) { throw 'Unexpected foundation file.' }
    $path = Join-Path $BinaryDirectory $file.path
    if ((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw 'Foundation hash mismatch.' }
}
New-Item -ItemType Directory -Path $output | Out-Null
& $InnoCompiler --no-ide-signtools ("--define=BinaryPath="+[IO.Path]::GetFullPath($BinaryDirectory)) ("--define=BundlePath="+[IO.Path]::GetFullPath($BundleDirectory)) ("--define=PackageName="+$manifest.package_filename) ("--define=FoundationVersion="+$foundation.bootstrapper_version) ("--define=OutputPath="+$output) (Join-Path $PSScriptRoot 'Setup.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup build failed.' }
