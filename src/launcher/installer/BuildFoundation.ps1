param(
    [Parameter(Mandatory=$true)][string]$BinaryDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$SigningTool
)
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
$identity = Get-SigningIdentity $SigningTool
$files = @(foreach ($name in @('MCWiiU-Launcher.exe','MCWiiU-Updater.exe')) {
    $path = Join-Path $BinaryDirectory $name
    Assert-RegularPath $path
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($path).ProductVersion -ne $identity.foundation_version) { throw 'Foundation executable version mismatch.' }
    [ordered]@{path=$name;size=(Get-Item -LiteralPath $path).Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$output = New-PackageOutput $OutputDirectory
Write-PackageJson (Join-Path $output 'foundation.unsigned.json') ([ordered]@{schema_version=1;product_id=$identity.product_id;app_id=$identity.app_id;bootstrapper_version=$identity.foundation_version;updater_protocol_version=2;files=$files})
