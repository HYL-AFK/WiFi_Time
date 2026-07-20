param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputDir = (Join-Path $PSScriptRoot '..\release')
)

$ErrorActionPreference = 'Stop'

function Require-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing build artifact: $Path"
    }
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$version = (Get-Content -Raw (Join-Path $root 'VERSION')).Trim()
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "VERSION must use MAJOR.MINOR.PATCH format"
}

$git = Get-Command git -ErrorAction SilentlyContinue
$commit = 'unknown'
if ($null -ne $git) {
    $gitPath = if ($git.Path) { $git.Path } else { $git.Source }
    $safeRoot = $root.Replace('\', '/')
    $commitOutput = & $gitPath -c "safe.directory=$safeRoot" -C $root rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $commitOutput) {
        $commit = ([string]$commitOutput).Trim()
    }
    if ([string]::IsNullOrWhiteSpace($commit)) { $commit = 'unknown' }
}

$appBin = Join-Path $BuildDir 'WiTime.bin'
$bootloader = Join-Path $BuildDir 'bootloader\bootloader.bin'
$partition = Join-Path $BuildDir 'partition_table\partition-table.bin'
$flasherArgs = Join-Path $BuildDir 'flasher_args.json'
Require-File $appBin
Require-File $bootloader
Require-File $partition

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$packageName = "WiTime-v$version-$commit"
$packageDir = Join-Path $OutputDir $packageName
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

Copy-Item $appBin (Join-Path $packageDir "$packageName.bin") -Force
Copy-Item $bootloader (Join-Path $packageDir 'bootloader.bin') -Force
Copy-Item $partition (Join-Path $packageDir 'partition-table.bin') -Force

$flashArgsText = @(
    '# ESP32-S3 WiTime flashing offsets'
    'bootloader.bin=0x0'
    'partition-table.bin=0x8000'
    "$packageName.bin=0x10000"
)
if (Test-Path -LiteralPath $flasherArgs -PathType Leaf) {
    $flashArgsText += ''
    $flashArgsText += '# Build-generated esptool arguments:'
    $flashArgsText += (Get-Content -Raw $flasherArgs).Trim()
}
Set-Content -LiteralPath (Join-Path $packageDir 'flash_args.txt') -Value $flashArgsText -Encoding UTF8

$builtAt = (Get-Date).ToUniversalTime().ToString('o')
$manifest = [ordered]@{
    project = 'WiTime'
    version = $version
    git_commit = $commit
    built_at_utc = $builtAt
    target = 'esp32s3'
    flash_size = '4MB'
    partition_scheme = 'single_app_large'
    artifacts = @(
        "$packageName.bin"
        'bootloader.bin'
        'partition-table.bin'
        'flash_args.txt'
    )
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $packageDir 'manifest.json') -Encoding UTF8
Set-Content -LiteralPath (Join-Path $packageDir 'version.txt') -Value "$version`n" -Encoding UTF8

$hashLines = Get-ChildItem -LiteralPath $packageDir -File |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object Name |
    ForEach-Object {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
        "$hash  $($_.Name)"
    }
Set-Content -LiteralPath (Join-Path $packageDir 'SHA256SUMS.txt') -Value $hashLines -Encoding ASCII

Write-Output "Created firmware package: $packageDir"
