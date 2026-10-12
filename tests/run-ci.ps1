param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ZigPath,
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
$profileSource = Join-Path $projectRoot 'src\profile.h'
$proxySource = Join-Path $projectRoot 'dist\dwmapi.dll'
foreach ($source in @($filterSource, $testSource, $mockSource, $loaderSource, $profileSource, $proxySource)) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw ('Required test input does not exist: ' + (Split-Path -Leaf $source))
    }
}
$profileText = [IO.File]::ReadAllText($profileSource)
if ($profileText -notmatch '(?m)^#define\s+PROFILE_SIZE\s+([0-9]+)u\s*$') {
    throw 'Cannot read PROFILE_SIZE from src/profile.h.'
}
$profileSize = [UInt32]::Parse($Matches[1])
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
    scope = 'Synthetic Win32 helper windows and mock Flutter loader only. Real Oopz UI, voice calls, ad display, and newer versions are not verified in CI.'
    dll = 'dist/dwmapi.dll'
    dllSha256 = (Get-FileHash -LiteralPath $proxySource -Algorithm SHA256).Hash.ToLowerInvariant()
    profileSize = $profileSize
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
    [IO.File]::WriteAllBytes((Join-Path $runtimeDirectory 'unsupported.so'), [byte[]](1, 2, 3, 4))
    $zeroFile = [IO.File]::Open((Join-Path $runtimeDirectory 'unsupported-size-matched.so'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $zeroFile.SetLength($profileSize) } finally { $zeroFile.Dispose() }
    foreach ($case in @('unsupported', 'unsupported-size-matched')) {
        foreach ($attempt in 1..2) {
            Invoke-StandaloneTest -Name ("loader-{0}-{1}" -f $case, $attempt) -Executable $loaderExecutable -Arguments ("{0}.so original" -f $case) -ExpectedMarker '(?m)^loader test \(original\): 0 failures, status=0x7\r?$'
        }
    }
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
