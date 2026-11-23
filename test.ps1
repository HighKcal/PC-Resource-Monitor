[CmdletBinding()]
param(
    [string]$WorkDirectory = (Join-Path $PSScriptRoot '.build'),
    [string]$ResultDirectory = (Join-Path $PSScriptRoot ('.test-results/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))),
    [switch]$SkipBuild
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools/BuildSupport.ps1')

# 시험 원본과 제품 원본은 src에서 직접 읽는다. 각 실행은 새 결과 폴더/시험 DB를 쓴다.
# 실제 Shell 배너, 브라우저, 외부 조회/번역 probe는 이 대표 회귀에 포함하지 않는다.
$sourceRoot = Join-Path $PSScriptRoot 'src'
$workRoot = [IO.Path]::GetFullPath($WorkDirectory)
$resultRoot = [IO.Path]::GetFullPath($ResultDirectory)
$productRoot = Join-Path $PSScriptRoot 'bin/win-x64'
$nativeRoot = Join-Path $workRoot 'native'
$testRoot = Join-Path $workRoot 'tests'
$dotnet = Get-Dotnet
if (Test-Path -LiteralPath $resultRoot) { throw 'ResultDirectory must be a new directory so stale DBs and results cannot be reused.' }
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') -WorkDirectory $workRoot }
Initialize-CCompiler
foreach ($required in @((Join-Path $productRoot 'PcResourceMonitor.dll'), (Join-Path $productRoot 'MonitorBridge.dll'), (Join-Path $nativeRoot 'engine_monitor.obj'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw ('Build outputs missing; run build.ps1 with the same WorkDirectory: ' + $required) }
}
New-Item -ItemType Directory -Path $resultRoot, $testRoot -Force | Out-Null
$managedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $productRoot 'PcResourceMonitor.dll')).Hash
$nativeHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $productRoot 'MonitorBridge.dll')).Hash
$checks = New-Object 'System.Collections.Generic.List[object]'

function Run-Case {
    param([string]$Name, [string]$Executable, [string[]]$Arguments = @())
    Write-Host "TEST $Name"
    try {
        Invoke-Checked $Executable $Arguments
        $checks.Add([pscustomobject]@{ name = $Name; passed = $true })
    }
    catch {
        $checks.Add([pscustomobject]@{ name = $Name; passed = $false; error = $_.Exception.Message })
        throw
    }
}
function Build-CTest {
    param([string]$Name, [string]$Source, [string[]]$Objects = @(), [string[]]$Libraries = @(), [string]$Manifest = '')
    $object = Join-Path $testRoot ($Name + '.obj')
    $executable = Join-Path $testRoot ($Name + '.exe')
    Push-Location -LiteralPath $sourceRoot
    try { Invoke-Checked cl.exe (@(Get-CFlags '.') + @('/c', $Source, ('/Fo' + $object))) }
    finally { Pop-Location }
    $manifestFlags = @()
    if ($Manifest) { $manifestFlags = @('/MANIFEST:EMBED', ('/MANIFESTINPUT:' + $Manifest)) }
    Invoke-Checked link.exe (@('/nologo', ('/OUT:' + $executable), $object) + $Objects + $Libraries + $manifestFlags)
    return $executable
}
function Publish-Runner {
    param([string]$Project, [string]$OutputName, [string]$NativeDll)
    $output = Join-Path $testRoot $OutputName
    Invoke-Checked $dotnet @('publish', (Join-Path $PSScriptRoot ('tests/' + $Project + '.csproj')), '-c', 'Release', '-r', 'win-x64', '--self-contained', 'false',
        '--artifacts-path', (Join-Path $testRoot ($OutputName + '-artifacts')), ('-p:ProductDir=' + $productRoot), ('-p:TestNative=' + $NativeDll),
        '-p:DebugType=None', '-p:DebugSymbols=false', '-o', $output, '--nologo')
    if ($Project -ne 'CompanionRegression') {
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $output 'PcResourceMonitor.dll')).Hash -ne $managedHash) { throw 'Runner copied a different product assembly.' }
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $output 'MonitorBridge.dll')).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $NativeDll).Hash) { throw 'Runner copied a different native DLL.' }
    return (Join-Path $output ($Project + '.dll'))
}

# 어댑터는 합성 입력의 한 조건만 주입한다. bridge 외의 번역 단위는 제품 오브젝트를 연결한다.
function Build-BridgeFixture {
    param([string]$Name, [string]$Source, [string[]]$SupportObjects = @())
    $directory = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $object = Join-Path $directory 'test_native.obj'
    $dll = Join-Path $directory 'MonitorBridge.dll'
    Push-Location -LiteralPath $sourceRoot
    try { Invoke-Checked cl.exe (@(Get-CFlags '.') + @('/c', $Source, ('/Fo' + $object))) }
    finally { Pop-Location }
    $objects = @($object, (Join-Path $nativeRoot 'sqlite3.obj')) + $SupportObjects
    foreach ($unit in Get-NativeUnits) {
        if ($unit -ne 'wpf/native/bridge') { $objects += Join-Path $nativeRoot (($unit -replace '/', '_') + '.obj') }
    }
    Invoke-Checked link.exe (@('/nologo', '/DLL', ('/OUT:' + $dll), ('/IMPLIB:' + (Join-Path $directory 'MonitorBridge.lib')),
        ('/DEF:' + (Join-Path $sourceRoot 'wpf/native/MonitorBridge.def'))) + $objects + @(Get-NativeLibraries))
    return $dll
}

try {
    # 순수 차분과 정책은 생산 오브젝트를 연결한다. Shell/GPU 경계는 기존 fixture가
    # 제품 구현을 직접 포함하며 실제 Shell IPC나 GPU 하드웨어 없이 검사한다.
    $test = Build-CTest 'collector-core' 'tests/collector/test_core.c' @((Join-Path $nativeRoot 'collector_resource_core.obj'))
    Run-Case 'collector counter validity and units' $test
    $test = Build-CTest 'notification-policy' 'tests/notifications/test_notification_policy.c' @((Join-Path $nativeRoot 'notifications_notification_policy.obj'))
    Run-Case 'notification policy and preserved evidence' $test
    $test = Build-CTest 'notification-shell' 'tests/notifications/test_notification_shell.c' @() @('user32.lib')
    Run-Case 'Shell adapter fake API lifecycle' $test
    $test = Build-CTest 'system-usage' 'tests/wpf/test_system_usage.c' @() @(Get-NativeLibraries)
    Run-Case 'GPU engine attribution fixture' $test

    # 실제 bridge/engine/SQLite 생산 오브젝트와 시험 내부 접근 어댑터를 연결한다.
    # DB 이름·종료 후 삭제는 기존 test_recent가 담당한다.
    $fixtureObject = Join-Path $testRoot 'monitor_fixture.obj'
    Push-Location -LiteralPath $sourceRoot
    try { Invoke-Checked cl.exe (@(Get-CFlags '.') + @('/c', 'tests/support/monitor_fixture.c', ('/Fo' + $fixtureObject))) }
    finally { Pop-Location }
    $recentObjects = @($fixtureObject, (Join-Path $nativeRoot 'sqlite3.obj'))
    foreach ($unit in Get-NativeUnits) {
        if ($unit -notin @('wpf/native/companion', 'wpf/native/system_usage')) { $recentObjects += Join-Path $nativeRoot (($unit -replace '/', '_') + '.obj') }
    }
    # 이 fixture는 GetTempPathA/DeleteFileA를 쓰므로 이 시험 exe에만 UTF-8 코드 페이지를 설정한다.
    # 한글 TEMP의 경로 바이트를 제품의 UTF-8 DB 계약과 맞추며 제품 manifest는 바꾸지 않는다.
    $test = Build-CTest 'recent-native' 'tests/recent-comparison/test_recent.c' $recentObjects @(Get-NativeLibraries) -Manifest (Join-Path $PSScriptRoot 'tests/recent-native.manifest')
    Run-Case 'native recent 4/5, cutoff, missing values and clock reversal' $test

    # runner는 일반 권한으로 실행하지만 참조하는 assembly/native 바이트는 생산본이다.
    $productNative = Join-Path $productRoot 'MonitorBridge.dll'
    $runner = Publish-Runner 'RecentComparison' 'recent-wpf' $productNative
    Run-Case 'WPF recent comparison and evidence priority' $dotnet @($runner, (Join-Path $resultRoot 'recent-wpf'), $managedHash)
    $runner = Publish-Runner 'RecentFailures' 'recent-failures' $productNative
    Run-Case 'recent request diagnostics, stale selection and database recovery' $dotnet @($runner, (Join-Path $resultRoot 'recent-failures'), $managedHash)
    # 동일 tuple 중복 행과 세션별 그룹 대상 안내는 배포된 두 DLL을 그대로 읽는다.
    # 두 WPF runner는 다른 GUI 회귀와 순차 실행해 포커스·메뉴 작업이 겹치지 않게 한다.
    $runner = Publish-Runner 'DuplicateRows' 'duplicate-rows' $productNative
    Run-Case 'duplicate process row display selection and command identity boundaries' $dotnet @($runner, (Join-Path $resultRoot 'duplicate-rows'), $managedHash, $nativeHash)
    $runner = Publish-Runner 'SessionGroupStatus' 'session-group-status' $productNative
    Run-Case 'new session group target status and late response lifetime' $dotnet @($runner, (Join-Path $resultRoot 'session-group-status'), $managedHash, $nativeHash)
    # 실제 bridge를 포함한 시험 어댑터만 monitor_ingest 입력의 CPU 유효성을 한 번 바꾼다.
    # 나머지 native 오브젝트와 managed DLL은 위에서 만든 생산본이며 bin은 수정하지 않는다.
    $completedNative = Build-BridgeFixture 'completed-fixture-native' 'tests/wpf/completed-evidence/test_native.c'
    $runner = Publish-Runner 'CompletedEvidence' 'completed-evidence' $completedNative
    Run-Case 'completed evidence across current CPU missing, recovery and next window' $dotnet @($runner, (Join-Path $resultRoot 'completed-evidence'), $managedHash)
    # 피드백 시각만 보관 기한 너머로 옮겨 같은 화면 회차에서 선택한 최근 근거가 바뀌는지 확인한다.
    $retentionNative = Build-BridgeFixture 'recent-retention-native' 'tests/wpf/recent-comparison/test_retention_native.c' @($fixtureObject)
    $runner = Publish-Runner 'RecentFailures' 'recent-retention' $retentionNative
    Run-Case 'selected recent evidence after feedback retention' $dotnet @($runner, (Join-Path $resultRoot 'recent-retention'), $managedHash, '--retention-native')
    # 소속 변경은 실제 정책을 호출하고 표시 그룹의 첫 calloc만 시험 DLL에서 실패시킨다.
    # bridge/grouping 두 번역 단위만 대체하며 제품과 기존 fixture 출력은 덮어쓰지 않는다.
    $groupRoot = Join-Path $testRoot 'group-refresh-native'
    New-Item -ItemType Directory -Path $groupRoot -Force | Out-Null
    $groupObject = Join-Path $groupRoot 'test_native.obj'
    $groupNative = Join-Path $groupRoot 'MonitorBridge.dll'
    Push-Location -LiteralPath $sourceRoot
    try { Invoke-Checked cl.exe (@(Get-CFlags '.') + @('/c', 'tests/wpf/group-refresh/test_native.c', ('/Fo' + $groupObject))) }
    finally { Pop-Location }
    $groupObjects = @($groupObject, (Join-Path $testRoot 'monitor_fixture.obj'), (Join-Path $nativeRoot 'sqlite3.obj'))
    foreach ($unit in Get-NativeUnits) {
        if ($unit -notin @('wpf/native/bridge', 'grouping/program_groups')) { $groupObjects += Join-Path $nativeRoot (($unit -replace '/', '_') + '.obj') }
    }
    Invoke-Checked link.exe (@('/nologo', '/DLL', ('/OUT:' + $groupNative), ('/IMPLIB:' + (Join-Path $groupRoot 'MonitorBridge.lib')),
        ('/DEF:' + (Join-Path $sourceRoot 'wpf/native/MonitorBridge.def'))) + $groupObjects + @(Get-NativeLibraries))
    $runner = Publish-Runner 'GroupRefresh' 'group-refresh' $groupNative
    Run-Case 'group edit partial success, refresh retry and independent status preservation' $dotnet @($runner, (Join-Path $resultRoot 'group-refresh'), $managedHash)
    $runner = Publish-Runner 'NotificationNavigation' 'navigation' $productNative
    Run-Case 'candidate notification click navigation' $dotnet @($runner, (Join-Path $resultRoot 'navigation.json'))
    $runner = Publish-Runner 'StorageFailures' 'storage-failures' $productNative
    foreach ($mode in @('retry', 'ingest', 'feedback')) {
        Run-Case ('storage failure ' + $mode) $dotnet @($runner, $mode, (Join-Path $resultRoot ('storage-' + $mode + '.sqlite')))
    }
    $runner = Publish-Runner 'ProgramInfoDisplay' 'program-info-production' $productNative
    Run-Case 'program info production native local evidence' $dotnet @($runner, (Join-Path $resultRoot 'program-info-production'), $managedHash, '--production-native')

    # 지연/취소/동의 사례 전용 DLL은 별도 출력에만 만든다. 생산 bin에는 복사하지 않는다.
    $fixtureRoot = Join-Path $testRoot 'companion-fixture-native'
    New-Item -ItemType Directory -Path $fixtureRoot -Force | Out-Null
    $fixtureObject = Join-Path $fixtureRoot 'test_native.obj'
    $fixtureNative = Join-Path $fixtureRoot 'MonitorBridge.dll'
    Push-Location -LiteralPath $sourceRoot
    try { Invoke-Checked cl.exe (@(Get-CFlags '.') + @('/c', 'tests/wpf/program-info-display/test_native.c', ('/Fo' + $fixtureObject))) }
    finally { Pop-Location }
    Invoke-Checked link.exe (@('/nologo', '/DLL', ('/OUT:' + $fixtureNative), ('/IMPLIB:' + (Join-Path $fixtureRoot 'MonitorBridge.lib')), $fixtureObject, (Join-Path $nativeRoot 'notifications_notification_shell.obj')) + @(Get-NativeLibraries))
    $runner = Publish-Runner 'ProgramInfoDisplay' 'program-info-fixture' $fixtureNative
    Run-Case 'program info fixture controls and preview' $dotnet @($runner, (Join-Path $resultRoot 'program-info-fixture'), $managedHash)
    $runner = Publish-Runner 'CompanionRegression' 'companion-lifetime' $fixtureNative
    Run-Case 'companion worker lifetime and stale result rejection' $dotnet @($runner)
}
finally {
    # 실패한 사례도 같은 보고서에 남긴다. WPF 회귀 DB·이미지·보고서는 ResultDirectory에 둔다.
    # C recent fixture의 별도 DB는 프로세스 TEMP에 생성되며 해당 시험이 종료 전에 삭제한다.
    $summary = [pscustomobject]@{
        productManagedSha256 = $managedHash
        productNativeSha256 = $nativeHash
        cases = @($checks.ToArray())
        actualShellClick = $false
        externalLookupOrTranslation = $false
    }
    $json = $summary | ConvertTo-Json -Depth 6
    [IO.File]::WriteAllText((Join-Path $resultRoot 'run-summary.json'), $json, (New-Object Text.UTF8Encoding($false)))
}
Write-Host "Passed: $($checks.Count) representative cases; results: $resultRoot"
