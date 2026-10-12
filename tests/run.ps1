param(
    [string]$ZigPath = "$PSScriptRoot\..\toolchain\zig-0.16.0\ziglang\zig.exe",
    [string]$AotPath = (Join-Path $env:LOCALAPPDATA 'oopz\data\app.so'),
    [string]$ClientRoot = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath "$PSScriptRoot\..").Path
$zigExecutable = (Get-Item -LiteralPath $ZigPath).FullName
$originalAot = (Get-Item -LiteralPath $AotPath).FullName
$originalHash = (Get-FileHash -LiteralPath $originalAot -Algorithm SHA256).Hash
$runtime = Join-Path $PSScriptRoot 'runtime'
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
$tests = [Collections.Generic.List[object]]::new()
$savedGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$savedLocalCache = $env:ZIG_LOCAL_CACHE_DIR
$env:ZIG_GLOBAL_CACHE_DIR = "$projectRoot\toolchain\cache"
$env:ZIG_LOCAL_CACHE_DIR = "$projectRoot\toolchain\local-cache"

function Invoke-Check {
    param([string]$Name, [string]$Executable, [string]$Arguments, [string]$Marker, [bool]$OnStderr = $false)
    $stdoutPath = Join-Path $runtime ($Name + '-stdout.txt')
    $stderrPath = Join-Path $runtime ($Name + '-stderr.txt')
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $runtime -WindowStyle Hidden -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
    try {
        if (-not $process.WaitForExit(60000)) { Stop-Process -Id $process.Id -Force; throw ('Timed out: ' + $Name) }
        $process.WaitForExit()
        $stdout = [IO.File]::ReadAllText($stdoutPath).TrimEnd()
        $stderr = [IO.File]::ReadAllText($stderrPath).TrimEnd()
        $evidence = if ($OnStderr) { $stderr } else { $stdout }
        $passed = $process.ExitCode -eq 0 -and $evidence -match $Marker
        $tests.Add([ordered]@{ name = $Name; passed = $passed; exitCode = $process.ExitCode; stdout = $stdout; stderr = $stderr })
        Write-Host ($Name + ': ' + $stdout)
        if (-not $passed) { throw ('Failed: ' + $Name + ' ' + $stderr) }
    } finally { $process.Dispose() }
}

Push-Location $projectRoot
try {
    & $zigExecutable cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Werror src/snapshot_patch.c tests/snapshot_probe.c -o "$runtime\snapshot_probe.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed.' }
    & $zigExecutable cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Werror -shared tests/mock_flutter.c ("-Wl,--out-implib,$runtime\libflutter_windows.a") -o "$runtime\flutter_windows.dll"
    if ($LASTEXITCODE -ne 0) { throw 'Mock engine compilation failed.' }
    & $zigExecutable cc -target x86_64-windows-gnu -municode -O2 -Wall -Wextra -Werror tests/loader.c "$runtime\libflutter_windows.a" -ldwmapi -o "$runtime\oopz.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Loader compilation failed.' }
    Copy-Item -LiteralPath dist/dwmapi.dll -Destination "$runtime\dwmapi.dll" -Force
    Remove-Item -LiteralPath "$runtime\oopz_guard.ini" -ErrorAction SilentlyContinue
    Invoke-Check 'real-scan' "$runtime\snapshot_probe.exe" ('"' + $originalAot + '" "' + $runtime + '\real.patched.so"') '"recognized":true'
    $scan = [IO.File]::ReadAllText((Join-Path $runtime 'real-scan-stdout.txt')) | ConvertFrom-Json
    if ($scan.patched -le 0) { throw 'No advertising strings in this real snapshot; patched-loader cases cannot be exercised.' }
    foreach ($attempt in 1..2) {
        Invoke-Check ('real-loader-' + $attempt) "$runtime\oopz.exe" ('"' + $originalAot + '" patched real.patched.so') 'loader test \(patched\): 0 failures, status=0xf'
    }
    # Alter a non-advertising trailing byte: no original-file SHA allowlist.
    $changed = [IO.File]::ReadAllBytes($originalAot)
    $changed[$changed.Length - 1] = $changed[$changed.Length - 1] -bxor 1
    [IO.File]::WriteAllBytes("$runtime\changed.so", $changed)
    Invoke-Check 'changed-scan' "$runtime\snapshot_probe.exe" 'changed.so changed.patched.so' '"recognized":true'
    Invoke-Check 'changed-loader' "$runtime\oopz.exe" 'changed.so patched changed.patched.so' 'loader test \(patched\): 0 failures, status=0xf'
    [IO.File]::WriteAllBytes("$runtime\unsupported.so", [byte[]](1, 2, 3, 4))
    Invoke-Check 'invalid-loader' "$runtime\oopz.exe" 'unsupported.so original' 'loader test \(original\): 0 failures, status=0x7'
    [IO.File]::WriteAllText("$runtime\oopz_guard.ini", "[Guard]`r`nBlockAds=0`r`nHideOBSWindows=1`r`n")
    Invoke-Check 'privacy-only' "$runtime\oopz.exe" ('"' + $originalAot + '" disabled') 'loader test \(disabled\): 0 failures, status=0xf'
    Remove-Item -LiteralPath "$runtime\oopz_guard.ini"
    if (-not [string]::IsNullOrWhiteSpace($ClientRoot)) {
        $engineRoot = (Get-Item -LiteralPath $ClientRoot).FullName
        foreach ($helper in @('real_engine_smoke', 'aot_parse_smoke')) {
            & $zigExecutable cc -target x86_64-windows-gnu -municode -O2 -Wall -Wextra -Werror ("tests/" + $helper + '.c') -lole32 -o ("$runtime\" + $helper + '.exe')
            if ($LASTEXITCODE -ne 0) { throw ('Engine helper compilation failed: ' + $helper) }
        }
        Invoke-Check 'real-engine' "$runtime\real_engine_smoke.exe" ('"' + $engineRoot + '" "' + $runtime + '\real.patched.so"') 'PASS: real Flutter engine created/destroyed'
        Invoke-Check 'real-aot-parse' "$runtime\aot_parse_smoke.exe" ('"' + $engineRoot + '" "' + $runtime + '\real.patched.so"') 'Could not resolve main entrypoint function' $true
    }
    $unchanged = (Get-FileHash -LiteralPath $originalAot -Algorithm SHA256).Hash -eq $originalHash
    if (-not $unchanged) { throw 'Original source unexpectedly changed.' }
    $result = [ordered]@{
        passed = $true
        originalUnchanged = $unchanged
        dllSha256 = (Get-FileHash -LiteralPath dist/dwmapi.dll -Algorithm SHA256).Hash.ToLowerInvariant()
        scan = $scan
        realEngineTested = -not [string]::IsNullOrWhiteSpace($ClientRoot)
        tests = @($tests.ToArray())
    }
    [IO.File]::WriteAllText("$projectRoot\dist\local-test-results.json", ($result | ConvertTo-Json -Depth 6) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
} finally {
    Remove-Item -LiteralPath "$runtime\oopz_guard.ini" -ErrorAction SilentlyContinue
    [Environment]::SetEnvironmentVariable('ZIG_GLOBAL_CACHE_DIR', $savedGlobalCache, 'Process')
    [Environment]::SetEnvironmentVariable('ZIG_LOCAL_CACHE_DIR', $savedLocalCache, 'Process')
    Pop-Location
}
