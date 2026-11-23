Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 모든 경로는 배포 폴더나 명시한 출력 폴더에서 계산한다. native 실행 실패는 즉시 전파한다.
function Invoke-Checked {
    param([Parameter(Mandatory)][string]$Executable, [string[]]$Arguments = @())
    & $Executable @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
function Initialize-CCompiler {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $locator)) { throw 'Install Visual Studio C++ x64 Build Tools, or use an x64 Native Tools prompt.' }
    $installation = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'The Visual Studio C++ x64 toolchain was not found.' }
    $setup = Join-Path ([string]$installation) 'VC\Auxiliary\Build\vcvars64.bat'
    # Windows PowerShell 5.1의 native 인자 재인용을 거치지 않고 cmd 명령문을 전달한다.
    # /u는 SET의 출력을 UTF-16으로 만들어 한글이 포함된 PATH/INCLUDE도 그대로 읽게 한다.
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $env:ComSpec
    $start.Arguments = '/d /u /c call "' + $setup + '" >nul && set'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::Unicode
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Failed to start the Visual Studio environment setup.' }
        $environmentText = $process.StandardOutput.ReadToEnd()
        $setupError = $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw ('Failed to initialize the Visual Studio x64 environment. ' + $setupError) }
    }
    finally { $process.Dispose() }
    foreach ($line in ($environmentText -split '\r?\n')) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) { [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process') }
    }
}
function Get-CFlags {
    param([string]$SourceRoot)
    $flags = @('/nologo', '/std:c17', '/W4', '/WX', '/O2', '/MT', '/utf-8', '/D_WIN32_WINNT=0x0601', '/DPSAPI_VERSION=2', '/DUNICODE', '/D_UNICODE')
    foreach ($folder in @('include', 'collector', 'engine', 'storage', 'detection', 'grouping', 'gui', 'notifications', 'third_party/sqlite')) { $flags += '/I' + (Join-Path $SourceRoot $folder) }
    return $flags
}
function Get-NativeUnits {
    # WPF에서 실제 사용하는 C 단위만 포함한다. Win32 창·실험 CLI는 이 제품의 빌드 대상이 아니다.
    return @('engine/monitor', 'engine/monitor_window', 'storage/monitor_storage', 'storage/monitor_source', 'storage/monitor_history',
        'detection/baseline', 'detection/baseline_history', 'collector/resource_core', 'collector/resource_collector', 'collector/monitor_identity',
        'grouping/process_grouping', 'grouping/process_grouping_windows', 'grouping/program_groups',
        'notifications/notification_policy', 'notifications/notification_shell', 'wpf/native/bridge', 'wpf/native/companion', 'wpf/native/system_usage')
}
function Get-NativeLibraries {
    return @('user32.lib', 'shell32.lib', 'advapi32.lib', 'version.lib', 'crypt32.lib', 'wintrust.lib', 'winhttp.lib', 'ws2_32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib', 'pdh.lib')
}
function Build-Native {
    param([string]$SourceRoot, [string]$OutputDirectory)
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $flags = @(Get-CFlags '.')
    $objects = @()
    Push-Location -LiteralPath $SourceRoot
    try {
        foreach ($unit in Get-NativeUnits) {
            $object = Join-Path $OutputDirectory (($unit -replace '/', '_') + '.obj')
            Invoke-Checked cl.exe ($flags + @('/c', ($unit + '.c'), ('/Fo' + $object)))
            $objects += $object
        }
        $sqlite = Join-Path $OutputDirectory 'sqlite3.obj'
        Invoke-Checked cl.exe @('/nologo', '/std:c17', '/O2', '/MT', '/w', '/DSQLITE_THREADSAFE=1', '/DSQLITE_DQS=0', '/DSQLITE_DEFAULT_MEMSTATUS=0', '/DSQLITE_ENABLE_DBSTAT_VTAB', '/c', 'third_party/sqlite/sqlite3.c', ('/Fo' + $sqlite))
        $objects += $sqlite
        Invoke-Checked link.exe (@('/nologo', '/DLL', ('/OUT:' + (Join-Path $OutputDirectory 'MonitorBridge.dll')), ('/IMPLIB:' + (Join-Path $OutputDirectory 'MonitorBridge.lib')), '/DEF:wpf/native/MonitorBridge.def') + $objects + @(Get-NativeLibraries))
    }
    finally { Pop-Location }
}
function Get-Dotnet {
    $command = Get-Command dotnet.exe -CommandType Application -ErrorAction Stop
    return $command.Source
}
