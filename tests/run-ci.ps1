param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ZigPath,
    [string]$PythonPath = 'python',
    [string]$ResultPath = (Join-Path $PSScriptRoot '..\dist\ci-test-results.json')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'The window filter tests require 64-bit Windows and a usable Win32 window station.'
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$zigExecutable = (Get-Item -LiteralPath $ZigPath).FullName
$filterSource = Join-Path $projectRoot 'src\window_filter.c'
$testSource = Join-Path $projectRoot 'src\window_filter_test.c'
$mockSource = Join-Path $projectRoot 'tests\mock_flutter.c'
$loaderSource = Join-Path $projectRoot 'tests\loader.c'
$patchSource = Join-Path $projectRoot 'src\snapshot_patch.c'
$probeSource = Join-Path $projectRoot 'tests\snapshot_probe.c'
$adaptiveTest = Join-Path $projectRoot 'tools\test_adaptive.py'
$proxySource = Join-Path $projectRoot 'dist\dwmapi.dll'
foreach ($source in @($filterSource, $testSource, $mockSource, $loaderSource, $patchSource, $probeSource, $adaptiveTest, $proxySource)) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw ('Required test input does not exist: ' + (Split-Path -Leaf $source))
    }
}
$resultFile = [IO.Path]::GetFullPath($ResultPath)
if (-not (Test-Path -LiteralPath (Split-Path -Parent $resultFile) -PathType Container)) {
    throw 'The CI result destination directory must already exist.'
}

# Hosted Windows runners supply RUNNER_TEMP; local runs stay in this workspace.
$runtimeParent = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $PSScriptRoot
} else {
    (Resolve-Path -LiteralPath $env:RUNNER_TEMP).Path
}
$runtimeParent = [IO.Path]::GetFullPath($runtimeParent)
$runtimeDirectory = [IO.Path]::GetFullPath((Join-Path $runtimeParent ('.ci-window-filter-' + [Guid]::NewGuid().ToString('N'))))
$expectedPrefix = $runtimeParent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $runtimeDirectory.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The test runtime directory must remain within the selected temporary directory.'
}
New-Item -ItemType Directory -Path $runtimeDirectory | Out-Null

$savedGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$savedLocalCache = $env:ZIG_LOCAL_CACHE_DIR
$testProcess = $null
$succeeded = $false
$testExecutable = Join-Path $runtimeDirectory 'window_filter_test.exe'
$helperExecutable = Join-Path $runtimeDirectory 'obs64.exe'
$testResults = [Collections.Generic.List[object]]::new()
$result = [ordered]@{
    schemaVersion = 1
    passed = $false
    target = 'x86_64-windows-gnu'
    scope = 'Adaptive ELF/Dart fixtures, synthetic Win32 windows and actual proxy DLL with mock Flutter. Real Oopz UI and voice calls require manual verification.'
    dll = 'dist/dwmapi.dll'
    dllSha256 = (Get-FileHash -LiteralPath $proxySource -Algorithm SHA256).Hash.ToLowerInvariant()
    adaptivePolicy = 2
    tests = @()
}

function Invoke-StandaloneTest {
    param([string]$Name, [string]$Executable, [string]$Arguments, [string]$ExpectedMarker)
    $stdoutPath = Join-Path $runtimeDirectory ($Name + '-stdout.txt')
    $stderrPath = Join-Path $runtimeDirectory ($Name + '-stderr.txt')
    $startParameters = @{
        FilePath = $Executable
        WorkingDirectory = $runtimeDirectory
        WindowStyle = 'Hidden'
        RedirectStandardOutput = $stdoutPath
        RedirectStandardError = $stderrPath
        PassThru = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($Arguments)) { $startParameters.ArgumentList = $Arguments }
    $script:testProcess = Start-Process @startParameters
    if (-not $script:testProcess.WaitForExit(60000)) {
        throw 'A standalone CI test exceeded the 60-second time limit.'
    }
    $script:testProcess.WaitForExit()
    $exitCode = $script:testProcess.ExitCode
    $stdout = [IO.File]::ReadAllText($stdoutPath).TrimEnd()
    $stderr = [IO.File]::ReadAllText($stderrPath).TrimEnd()
    $passed = $exitCode -eq 0 -and $stdout -match $ExpectedMarker
    $testResults.Add([ordered]@{ name = $Name; passed = $passed; exitCode = $exitCode; stdout = $stdout; stderr = $stderr })
    Write-Host ("{0}: {1}" -f $Name, $stdout)
    if (-not [string]::IsNullOrWhiteSpace($stderr)) { Write-Host $stderr }
    $script:testProcess.Dispose()
    $script:testProcess = $null
    if (-not $passed) { throw "Standalone CI test failed: $Name (exit code $exitCode)." }
}

try {
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $runtimeDirectory 'zig-global-cache'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $runtimeDirectory 'zig-local-cache'
    Write-Host 'Building standalone synthetic Win32 window filter tests.'
    & $zigExecutable cc -target x86_64-windows-gnu -std=c11 -O2 -Wall -Wextra -Werror $filterSource $testSource -luser32 -lkernel32 -o $testExecutable
    if ($LASTEXITCODE -ne 0) {
        throw "Window filter test compilation failed (exit code $LASTEXITCODE)."
    }

    # The helper uses hidden test windows; ordinary control windows also remain
    # hidden. EnumWindows callbacks only count handles from this test.
    Write-Host 'Running synthetic helper window tests; this does not test the Oopz client or its UI.'
    Invoke-StandaloneTest -Name 'window-filter' -Executable $testExecutable -Arguments '' -ExpectedMarker '(?m)^window_filter tests: 0 failures\r?$'

    Write-Host 'Building a mock Flutter loader to check actual directory DLL loading and unsupported snapshot pass-through.'
    $mockDll = Join-Path $runtimeDirectory 'flutter_windows.dll'
    $mockImport = Join-Path $runtimeDirectory 'libflutter_windows.a'
    $loaderExecutable = Join-Path $runtimeDirectory 'oopz.exe'
    & $zigExecutable cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Werror -shared $mockSource ("-Wl,--out-implib,$mockImport") -o $mockDll
    if ($LASTEXITCODE -ne 0) { throw "Mock Flutter compilation failed (exit code $LASTEXITCODE)." }
    & $zigExecutable cc -target x86_64-windows-gnu -municode -O2 -Wall -Wextra -Werror $loaderSource $mockImport -ldwmapi -o $loaderExecutable
    if ($LASTEXITCODE -ne 0) { throw "Mock loader compilation failed (exit code $LASTEXITCODE)." }
    Copy-Item -LiteralPath $proxySource -Destination (Join-Path $runtimeDirectory 'dwmapi.dll')
    $result.dllSha256 = (Get-FileHash -LiteralPath (Join-Path $runtimeDirectory 'dwmapi.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    $probeExecutable = Join-Path $runtimeDirectory 'snapshot_probe.exe'
    & $zigExecutable cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Werror $patchSource $probeSource -o $probeExecutable
    if ($LASTEXITCODE -ne 0) { throw 'Adaptive scanner probe compilation failed.' }
    Invoke-StandaloneTest -Name 'adaptive-scanner' -Executable $PythonPath -Arguments ('"' + $adaptiveTest + '" --probe "' + $probeExecutable + '" --directory "' + $runtimeDirectory + '"') -ExpectedMarker 'adaptive scanner tests: 13 cases, 0 failures'
    foreach ($case in @('base', 'shifted', 'cid-changed', 'utf16', 'compressed-hash', 'separate-hash', 'wide-separate')) {
        foreach ($attempt in 1..2) {
            Invoke-StandaloneTest -Name ("loader-{0}-{1}" -f $case, $attempt) -Executable $loaderExecutable -Arguments ("{0}.so patched {0}.patched.so" -f $case) -ExpectedMarker '(?m)^loader test \(patched\): 0 failures, status=0xf\r?$'
        }
    }
    Invoke-StandaloneTest -Name 'loader-no-ads' -Executable $loaderExecutable -Arguments 'no-ads.so noads' -ExpectedMarker 'loader test \(noads\): 0 failures, status=0xf'
    foreach ($case in @('truncated', 'invalid-header', 'invalid-hashes', 'executable-only', 'overlapping-segments')) {
        Invoke-StandaloneTest -Name ("loader-{0}" -f $case) -Executable $loaderExecutable -Arguments ("{0}.so original" -f $case) -ExpectedMarker 'loader test \(original\): 0 failures, status=0x7'
    }
    $config = Join-Path $runtimeDirectory 'oopz_guard.ini'
    [IO.File]::WriteAllText($config, "[Guard]`r`nBlockAds=0`r`nHideOBSWindows=1`r`n")
    Invoke-StandaloneTest -Name 'loader-privacy-only' -Executable $loaderExecutable -Arguments 'base.so disabled' -ExpectedMarker 'loader test \(disabled\): 0 failures, status=0xf'
    [IO.File]::WriteAllText($config, "[Guard]`r`nBlockAds=1`r`nHideOBSWindows=0`r`n")
    Invoke-StandaloneTest -Name 'loader-ads-only' -Executable $loaderExecutable -Arguments 'base.so patched base.patched.so' -ExpectedMarker 'loader test \(patched\): 0 failures, status=0xf'
    [IO.File]::WriteAllText($config, "[Guard]`r`nBlockAds=0`r`nHideOBSWindows=0`r`n")
    Invoke-StandaloneTest -Name 'loader-disabled' -Executable $loaderExecutable -Arguments 'base.so disabled' -ExpectedMarker 'loader test \(disabled\): 0 failures, status=0xf'
    Remove-Item -LiteralPath $config
    # This cache belongs to the independently generated base fixture only.
    $fixtureHash = (Get-FileHash -LiteralPath (Join-Path $runtimeDirectory 'base.patched.so') -Algorithm SHA256).Hash.ToLowerInvariant()
    $fixtureCache = Join-Path ([IO.Path]::GetTempPath()) ('OopzGuard\snapshot-p2-' + $fixtureHash + '.so')
    if ((Get-FileHash -LiteralPath $fixtureCache -Algorithm SHA256).Hash.ToLowerInvariant() -ne $fixtureHash) { throw 'Unexpected fixture cache before corruption test.' }
    [IO.File]::WriteAllBytes($fixtureCache, [byte[]](1, 2, 3, 4))
    $lease = [IO.File]::Open($fixtureCache, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Invoke-StandaloneTest -Name 'loader-locked-damaged-cache' -Executable $loaderExecutable -Arguments 'base.so original' -ExpectedMarker 'loader test \(original\): 0 failures, status=0x7'
    } finally { $lease.Dispose() }
    Invoke-StandaloneTest -Name 'loader-cache-rebuild' -Executable $loaderExecutable -Arguments 'base.so patched base.patched.so' -ExpectedMarker 'loader test \(patched\): 0 failures, status=0xf'
    $succeeded = $true
} finally {
    # On abnormal exit, stop only processes created from this unique test path.
    if ($null -ne $testProcess -and -not $testProcess.HasExited) {
        Stop-Process -Id $testProcess.Id -Force -ErrorAction SilentlyContinue
    }
    Get-Process -Name obs64 -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Path -eq $helperExecutable) {
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
    }
    $result.passed = $succeeded
    $result.tests = @($testResults.ToArray())
    $json = $result | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText($resultFile, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    Write-Host ('CI result JSON: ' + (Split-Path -Leaf $resultFile))
    [Environment]::SetEnvironmentVariable('ZIG_GLOBAL_CACHE_DIR', $savedGlobalCache, 'Process')
    [Environment]::SetEnvironmentVariable('ZIG_LOCAL_CACHE_DIR', $savedLocalCache, 'Process')
    if ($null -ne $testProcess) { $testProcess.Dispose() }

    if ($succeeded) {
        # The absolute target and its parent were checked before creation.
        Remove-Item -LiteralPath $runtimeDirectory -Recurse -Force
    } else {
        Write-Host ('Test files and diagnostic output retained in temporary directory: ' + (Split-Path -Leaf $runtimeDirectory))
    }
}
