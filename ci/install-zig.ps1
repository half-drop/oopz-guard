$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'zig.json') -Raw | ConvertFrom-Json
$toolchainRoot = Join-Path $projectRoot 'toolchain'
New-Item -ItemType Directory -Path $toolchainRoot -Force | Out-Null
$archivePath = Join-Path $toolchainRoot ("zig-$($lock.version)-verified.zip")
$destination = Join-Path $toolchainRoot ("zig-$($lock.version)")
Invoke-WebRequest -Uri $lock.archive_url -OutFile $archivePath
if ((Get-Item -LiteralPath $archivePath).Length -ne $lock.archive_bytes) {
    throw 'Compiler archive size mismatch.'
}
$actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $lock.archive_sha256) { throw 'Compiler archive SHA256 mismatch.' }
Expand-Archive -LiteralPath $archivePath -DestinationPath $destination -Force
$zigPath = Join-Path $destination $lock.executable
$actualVersion = & $zigPath version
if ($LASTEXITCODE -ne 0 -or $actualVersion -ne $lock.version) { throw 'Compiler version mismatch.' }
Write-Host ("Verified Zig " + $actualVersion)
