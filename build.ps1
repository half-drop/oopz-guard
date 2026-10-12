param([string]$ZigPath = "$PSScriptRoot\toolchain\zig-0.16.0\ziglang\zig.exe")
$ErrorActionPreference = 'Stop'
$env:ZIG_GLOBAL_CACHE_DIR = "$PSScriptRoot\toolchain\cache"
$env:ZIG_LOCAL_CACHE_DIR = "$PSScriptRoot\toolchain\local-cache"
if (!(Test-Path -LiteralPath $ZigPath)) { throw 'Zig compiler not found. Supply -ZigPath.' }
New-Item -ItemType Directory -Path "$PSScriptRoot\dist" -Force | Out-Null
Push-Location $PSScriptRoot
try {
    & $ZigPath cc -target x86_64-windows-gnu -O2 -g0 -Wall -Wextra -Werror -shared src/oopz_guard.c src/snapshot_patch.c src/window_filter.c src/dwmapi_stubs.S src/dwmapi.def -lbcrypt -luser32 -o dist/dwmapi.dll
    if ($LASTEXITCODE -ne 0) { throw 'DLL compilation failed.' }
    Get-FileHash -LiteralPath "$PSScriptRoot\dist\dwmapi.dll" -Algorithm SHA256
} finally { Pop-Location }
