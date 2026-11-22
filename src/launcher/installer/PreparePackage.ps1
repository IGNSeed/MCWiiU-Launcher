param(
    [Parameter(Mandatory=$true)][string]$BinaryDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
. (Join-Path $PSScriptRoot 'PackageCommon.ps1')
$binaries = (Resolve-Path -LiteralPath $BinaryDirectory).Path
Assert-RegularPath $binaries
$package = Get-Content -LiteralPath (Join-Path $binaries 'launcher-package.json') -Raw | ConvertFrom-Json
if (!$package.native_core -or $package.developer_diagnostics -ne $false) { throw 'Native Release with developer diagnostics disabled is required.' }
$version = $package.version
if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $binaries 'MCWiiU Launcher.exe')).ProductVersion -ne $version) { throw 'Invalid or mismatched Launcher version.' }
$notices = Get-Content -LiteralPath (Join-Path $binaries 'license-inventory.json') -Raw | ConvertFrom-Json
if ($notices.schema_version -ne 1 -or $notices.notices.Count -lt 20) { throw 'Required notice inventory is incomplete.' }
$paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($notice in $notices.notices) {
    if ($notice.path -notmatch '^licenses/[A-Za-z0-9_.-]+$' -or !$paths.Add($notice.path)) { throw 'Invalid or duplicate notice path.' }
    $source = [IO.Path]::GetFullPath((Join-Path $workspace $notice.source))
    if (!$source.StartsWith($workspace+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Notice source outside workspace.' }
    $copied = Join-Path $binaries $notice.path
    Assert-RegularPath $source
    Assert-RegularPath $copied
    $originalHash = if ($notice.source_sha256) { $notice.source_sha256 } else { $notice.sha256 }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $originalHash -or (Get-FileHash -LiteralPath $copied -Algorithm SHA256).Hash.ToLowerInvariant() -ne $notice.sha256) { throw ('Missing or modified original notice: '+$notice.component) }
}
foreach ($required in @('Cemu','Discord-Social-SDK','saucer','glaze','coco','rebind','ereignis','flagpp','polo','lockpp','saucer-fill','WebView2','cubeb','ZArchive','imgui','ih264d','Vulkan-Headers','FontAwesome','openssl','curl','libpng','expat')) {
    if ($required -notin $notices.notices.component) { throw ('Missing required notice component: '+$required) }
}
$runtime = Join-Path $binaries 'discord_partner_sdk.dll'
if ((Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash -ne '0688B44B738148CB0DF9BECA00A64F2B8B351F87A7AF39035BC378008DF34F0D') { throw 'Pinned Discord Social SDK 1.11.24259 runtime is required.' }
$output = New-PackageOutput $OutputDirectory
$payload = Join-Path $output 'payload'
New-Item -ItemType Directory -Path (Join-Path $payload 'licenses') | Out-Null
foreach ($name in @('MCWiiU Launcher.exe','discord_partner_sdk.dll')) { Copy-Item -LiteralPath (Join-Path $binaries $name) -Destination $payload }
Copy-Item -LiteralPath (Join-Path $binaries 'license-inventory.json') -Destination (Join-Path $payload 'licenses/license-inventory.json')
if (Test-Path -LiteralPath (Join-Path $binaries 'WebView2Loader.dll')) { Copy-Item -LiteralPath (Join-Path $binaries 'WebView2Loader.dll') -Destination $payload }
foreach ($notice in $notices.notices) { Copy-Item -LiteralPath (Join-Path $binaries $notice.path) -Destination (Join-Path $payload $notice.path) }
$inventory = @(Get-PackageInventory $payload)
$name = "MCWiiU-Launcher-$version-win-x64.zip"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open((Join-Path $output $name), [IO.Compression.ZipArchiveMode]::Create)
try {
    # Windows PowerShellの.NET Frameworkでも、ZIP内の区切りを必ず '/' にする。
    foreach ($file in $inventory) { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $payload $file.path), $file.path, [IO.Compression.CompressionLevel]::Optimal) | Out-Null }
} finally { $zip.Dispose() }
$archive = Get-Item -LiteralPath (Join-Path $output $name)
Write-PackageJson (Join-Path $output 'package-plan.json') ([ordered]@{schema_version=1;version=$version;release_tag="v$version";package_filename=$name;package_size=$archive.Length;package_sha256=(Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant();files=$inventory})
Write-Output 'ZIP prepared. Finalize its manifest only after obtaining the matching GitHub release asset metadata.'
