param(
    [Parameter(Mandatory=$true)][string]$BinaryDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][UInt64]$ReleaseAssetId
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (!$output.StartsWith($workspace + '\.local\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Build output must be inside workspace .local.' }
if (Test-Path -LiteralPath $output) { throw 'Choose a new output directory.' }
$package = Get-Content -LiteralPath (Join-Path $BinaryDirectory 'launcher-package.json') -Raw | ConvertFrom-Json
if (!$package.native_core -or $ReleaseAssetId -eq 0) { throw 'Native Release and a release asset ID are required.' }
$version = $package.version
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Invalid version.' }
if ([Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $BinaryDirectory 'MCWiiU Launcher.exe')).ProductVersion -ne $version) { throw 'Launcher version mismatch.' }
# SDK DLL と同梱通知を欠いた更新パッケージは作成しない。
foreach ($required in @('discord_partner_sdk.dll','licenses/Discord-Social-SDK-NOTICES.txt')) {
    if (!(Test-Path -LiteralPath (Join-Path $BinaryDirectory $required) -PathType Leaf)) { throw ('Missing Discord SDK runtime or notices: ' + $required) }
}
$payload = Join-Path $output 'payload'
New-Item -ItemType Directory -Path (Join-Path $payload 'licenses') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'MCWiiU Launcher.exe') -Destination $payload
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'discord_partner_sdk.dll') -Destination $payload
if (Test-Path -LiteralPath (Join-Path $BinaryDirectory 'WebView2Loader.dll')) { Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'WebView2Loader.dll') -Destination $payload }
Copy-Item -LiteralPath (Join-Path $workspace 'LICENSE.txt') -Destination (Join-Path $payload 'licenses/Cemu-LICENSE.txt')
foreach ($license in Get-ChildItem -LiteralPath (Join-Path $BinaryDirectory 'licenses') -File) { Copy-Item -LiteralPath $license.FullName -Destination (Join-Path $payload ('licenses/' + $license.Name)) }
foreach ($license in Get-ChildItem -LiteralPath (Join-Path $workspace 'out/build/Release/vcpkg_installed/x64-windows-static/share') -Filter copyright -Recurse -File) {
    Copy-Item -LiteralPath $license.FullName -Destination (Join-Path $payload ('licenses/' + $license.Directory.Name + '-copyright.txt'))
}
$inventory = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=$_.FullName.Substring($payload.Length+1).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$name = "MCWiiU-Launcher-$version-win-x64.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($payload, (Join-Path $output $name))
$archive = Get-Item -LiteralPath (Join-Path $output $name)
$manifest = [ordered]@{
    schema_version=2; product_id='MCWiiU-Launcher'; channel='stable'; version=$version; architecture='x64'; release_tag="v$version"; release_asset_id=$ReleaseAssetId;
    package_filename=$name; package_size=$archive.Length; package_sha256=(Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); package_format='zip';
    required_bootstrapper_version='1.0.2'; required_updater_protocol_version=2;
    compatibility=[ordered]@{preferences_schema=2;history_schema=1;account_schema=1;game_data_schema=1;rollback_safe=$true}; files=$inventory
}
# 外部の安全な署名工程へ渡す正確なUTF-8 bytes。秘密鍵はこのscriptで生成しない。
[IO.File]::WriteAllText((Join-Path $output 'manifest.unsigned.json'), ($manifest | ConvertTo-Json -Depth 8 -Compress), [Text.UTF8Encoding]::new($false))
Write-Output "Package created. Sign manifest.unsigned.json externally as manifest.json / manifest.sig."
