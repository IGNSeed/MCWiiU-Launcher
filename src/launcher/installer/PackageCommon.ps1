$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
function Assert-RegularPath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        if ((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reparse paths are not packaging inputs or outputs.' }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}
function New-PackageOutput([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($workspace + '\.local\', [StringComparison]::OrdinalIgnoreCase) -or $full.Length -gt 220) { throw 'Choose a short output path inside workspace .local.' }
    Assert-RegularPath $full
    if (Test-Path -LiteralPath $full) { throw 'Choose a new output directory.' }
    New-Item -ItemType Directory -Path $full | Out-Null
    return $full
}
function Write-PackageJson([string]$Path, $Value) {
    if (Test-Path -LiteralPath $Path) { throw 'Refusing to replace existing metadata.' }
    [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12 -Compress), [Text.UTF8Encoding]::new($false))
}
function Get-PackageInventory([string]$Payload) {
    Assert-RegularPath $Payload
    @(Get-ChildItem -LiteralPath $Payload -Recurse -Force | ForEach-Object {
        if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked package entry.' }
        if (!$_.PSIsContainer) {
            [ordered]@{path=$_.FullName.Substring($Payload.Length+1).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
        }
    } | Sort-Object { $_.path })
}
function Get-SigningIdentity([string]$SigningTool) {
    $json = & $SigningTool identity
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read compiled signing identity.' }
    return ($json | ConvertFrom-Json)
}
