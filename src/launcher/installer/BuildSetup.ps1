param(
    [Parameter(Mandatory=$true)][string]$BinaryDirectory,
    [Parameter(Mandatory=$true)][string]$BundleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$InnoCompiler,
    [Parameter(Mandatory=$true)][string]$SigningTool,
    [Parameter(Mandatory=$true)][string]$PublicKeyFile
)
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
foreach ($path in @($BinaryDirectory,$BundleDirectory,$InnoCompiler,$SigningTool,$PublicKeyFile)) { Assert-RegularPath $path }
if ((Get-FileHash -LiteralPath $InnoCompiler -Algorithm SHA256).Hash -ne 'E0F9F1DC32032650C6629F6DEE41EB6D20BA140E4E3C07C25D8A24F9BA781644' -or (& $InnoCompiler --version).Trim() -ne '7.1.1') { throw 'Pinned Inno Setup 7.1.1 compiler required.' }
$identity = Get-SigningIdentity $SigningTool
$key = [IO.File]::ReadAllText([IO.Path]::GetFullPath($PublicKeyFile))
if ($key -cnotmatch '^[0-9a-f]{64}$' -or $identity.public_key -cne $key) { throw 'Setup requires the real verification key compiled into its matching build. Unconfigured trust is rejected.' }
# 暗号検証とZIP全entry・基盤EXE・展開payloadの照合を製品コードで行う。
& $SigningTool verify-bundle $PublicKeyFile $BundleDirectory $BinaryDirectory
if ($LASTEXITCODE -ne 0) { throw 'Signed setup bundle verification failed.' }
$manifest = Get-Content -LiteralPath (Join-Path $BundleDirectory 'manifest.json') -Raw | ConvertFrom-Json
$output = New-PackageOutput $OutputDirectory
& $InnoCompiler --no-ide-signtools ("--define=BinaryPath="+[IO.Path]::GetFullPath($BinaryDirectory)) ("--define=BundlePath="+[IO.Path]::GetFullPath($BundleDirectory)) ("--define=PackageName="+$manifest.package_filename) ("--define=FoundationVersion="+$identity.foundation_version) ("--define=LauncherVersion="+$manifest.version) ("--define=ProductId="+$identity.product_id) ("--define=ProductName="+$identity.product_name) ("--define=StableAppId="+$identity.app_id) ("--define=OutputPath="+$output) (Join-Path $PSScriptRoot 'Setup.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup build failed.' }
& $SigningTool verify-bundle $PublicKeyFile $BundleDirectory $BinaryDirectory
if ($LASTEXITCODE -ne 0) { throw 'Setup inputs changed during compilation.' }
Get-ChildItem -LiteralPath $output -Filter '*.exe' -File | Get-FileHash -Algorithm SHA256
