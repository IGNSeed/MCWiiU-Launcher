param(
    [Parameter(Mandatory=$true)][string]$PreparedDirectory,
    [Parameter(Mandatory=$true)][UInt64]$ReleaseAssetId,
    [Parameter(Mandatory=$true)][string]$AssetMetadata,
    [string]$RequiredFoundationVersion='1.0.2'
)
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path
if (!$prepared.StartsWith($workspace+'\.local\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Prepared package must be inside workspace .local.' }
Assert-RegularPath $prepared
if ((Test-Path -LiteralPath (Join-Path $prepared 'manifest.json')) -or (Test-Path -LiteralPath (Join-Path $prepared 'manifest.sig'))) { throw 'Never rewrite a signed manifest. Prepare and sign a new package transaction.' }
$plan = Get-Content -LiteralPath (Join-Path $prepared 'package-plan.json') -Raw | ConvertFrom-Json
$asset = Get-Content -LiteralPath $AssetMetadata -Raw | ConvertFrom-Json
if ($plan.schema_version -ne 1 -or $plan.version -notmatch '^\d+\.\d+\.\d+$' -or $RequiredFoundationVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid package plan or foundation version.' }
$name = "MCWiiU-Launcher-$($plan.version)-win-x64.zip"
$tag = "v$($plan.version)"
$url = "https://github.com/IGNSeed/MCWiiU-Launcher/releases/download/$tag/$name"
if ($ReleaseAssetId -eq 0 -or $ReleaseAssetId -gt 9007199254740991 -or $asset.id -ne $ReleaseAssetId -or $asset.name -cne $name -or $asset.tag_name -cne $tag -or $asset.browser_download_url -cne $url -or $plan.release_tag -cne $tag -or $plan.package_filename -cne $name) { throw 'Release asset identity, tag or official URL mismatch.' }
$archive = Get-Item -LiteralPath (Join-Path $prepared $name)
$hash = (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
if ($archive.Length -ne $plan.package_size -or $archive.Length -ne $asset.size -or $hash -cne $plan.package_sha256 -or $asset.digest -cne ('sha256:'+$hash)) { throw 'ZIP changed or does not match the uploaded asset digest and size.' }
$inventory = @(Get-PackageInventory (Join-Path $prepared 'payload'))
if (($inventory | ConvertTo-Json -Depth 8 -Compress) -cne ($plan.files | ConvertTo-Json -Depth 8 -Compress)) { throw 'Payload changed after ZIP preparation.' }
$productId = 'MCWiiU-Launcher'
$manifest = [ordered]@{
    schema_version=2;product_id=$productId;channel='stable';version=$plan.version;architecture='x64';release_tag=$tag;release_asset_id=$ReleaseAssetId;
    package_filename=$name;package_size=$archive.Length;package_sha256=$hash;package_format='zip';
    required_bootstrapper_version=$RequiredFoundationVersion;required_updater_protocol_version=2;
    compatibility=[ordered]@{preferences_schema=2;history_schema=1;account_schema=1;game_data_schema=1;rollback_safe=$true};files=$inventory
}
Write-PackageJson (Join-Path $prepared 'manifest.unsigned.json') $manifest
Write-Output 'Manifest finalized. Sign these exact bytes using MCWiiU-SigningTool sign manifest.'
