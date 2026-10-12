param(
    [string]$ZigPath = "$PSScriptRoot\..\toolchain\zig-0.16.0\ziglang\zig.exe",
    [string]$AotPath = (Join-Path $env:LOCALAPPDATA 'oopz\data\app.so')
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath "$PSScriptRoot\..").Path
$env:ZIG_GLOBAL_CACHE_DIR = "$projectRoot\toolchain\cache"
$env:ZIG_LOCAL_CACHE_DIR = "$projectRoot\toolchain\local-cache"
New-Item -ItemType Directory -Path "$PSScriptRoot\runtime" -Force | Out-Null
Push-Location $projectRoot
try {
    & $ZigPath cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Werror -shared tests/mock_flutter.c '-Wl,--out-implib,tests/runtime/libflutter_windows.a' -o tests/runtime/flutter_windows.dll
    if ($LASTEXITCODE -ne 0) { throw 'Mock engine build failed.' }
    & $ZigPath cc -target x86_64-windows-gnu -municode -O2 -Wall -Wextra -Werror tests/loader.c tests/runtime/libflutter_windows.a -ldwmapi -o tests/runtime/oopz.exe
    if ($LASTEXITCODE -ne 0) { throw 'Loader test build failed.' }
    Copy-Item -LiteralPath dist/dwmapi.dll -Destination tests/runtime/dwmapi.dll -Force
    & "$PSScriptRoot\runtime\oopz.exe" $AotPath patched
    if ($LASTEXITCODE -ne 0) { throw 'Exact-version loader test failed.' }
    & "$PSScriptRoot\runtime\oopz.exe" $AotPath patched
    if ($LASTEXITCODE -ne 0) { throw 'Cached-snapshot loader test failed.' }
    [IO.File]::WriteAllBytes("$PSScriptRoot\runtime\unsupported.so", [byte[]](1,2,3,4))
    & "$PSScriptRoot\runtime\oopz.exe" "$PSScriptRoot\runtime\unsupported.so" original
    if ($LASTEXITCODE -ne 0) { throw 'Unsupported-version pass-through failed.' }
    $tamperedAot = [IO.File]::ReadAllBytes($AotPath)
    $tamperedAot[$tamperedAot.Length - 1] = $tamperedAot[$tamperedAot.Length - 1] -bxor 1
    [IO.File]::WriteAllBytes("$PSScriptRoot\runtime\tampered.so", $tamperedAot)
    & "$PSScriptRoot\runtime\oopz.exe" "$PSScriptRoot\runtime\tampered.so" original
    if ($LASTEXITCODE -ne 0) { throw 'Same-size hash mismatch pass-through failed.' }
    try {
        [IO.File]::WriteAllText("$PSScriptRoot\runtime\oopz_guard.ini", "[Guard]`r`nBlockAds=0`r`nHideOBSWindows=0`r`n")
        & "$PSScriptRoot\runtime\oopz.exe" $AotPath disabled
        if ($LASTEXITCODE -ne 0) { throw 'Disabled settings pass-through failed.' }
    } finally {
        Remove-Item -LiteralPath "$PSScriptRoot\runtime\oopz_guard.ini" -ErrorAction SilentlyContinue
    }
} finally { Pop-Location }
