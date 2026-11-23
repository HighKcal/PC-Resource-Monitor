[CmdletBinding()]
param([string]$WorkDirectory = (Join-Path $PSScriptRoot '.build'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools/BuildSupport.ps1')

# src는 팀원이 수정하는 유일한 제품 원본이다. 빌드는 이를 읽고 출력 폴더만 갱신한다.
# 공식 bin은 항상 관리자 실행 manifest를 사용한다. 검증 runner의 manifest는 별도다.
$sourceRoot = Join-Path $PSScriptRoot 'src'
$workRoot = [IO.Path]::GetFullPath($WorkDirectory)
$productRoot = Join-Path $PSScriptRoot 'bin/win-x64'
$nativeRoot = Join-Path $workRoot 'native'
$dotnet = Get-Dotnet
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'
Initialize-CCompiler
Build-Native $sourceRoot $nativeRoot
Invoke-Checked $dotnet @('publish', (Join-Path $sourceRoot 'wpf/PcResourceMonitor.csproj'), '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
    '--artifacts-path', (Join-Path $workRoot 'managed'), '-p:ValidationAsInvoker=false',
    '-p:DebugType=None', '-p:DebugSymbols=false', '-p:IncludeSymbols=false', '-o', $productRoot, '--nologo')
Copy-Item -LiteralPath (Join-Path $nativeRoot 'MonitorBridge.dll') -Destination (Join-Path $productRoot 'MonitorBridge.dll') -Force
Write-Host "Built: $(Join-Path $productRoot 'PcResourceMonitor.exe')"
