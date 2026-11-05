param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Phase,
    [string]$OutputDirectory = 'build-perf-20261103',
    [string[]]$Modes = @('core', 'experiment'),
    [string[]]$Cases = @(),
    [switch]$AllWrapperCases,
    [switch]$Spread,
    [switch]$Profile
)
$ErrorActionPreference = 'Stop'
$matrix = @(
    @{ Name = 'initial100-distinct'; Instances = 100; Days = 0; Kind = 'distinct' },
    @{ Name = 'initial500-mixed'; Instances = 500; Days = 0; Kind = 'mixed' },
    @{ Name = 'mature100-distinct'; Instances = 100; Days = 14; Kind = 'distinct' },
    @{ Name = 'mature500-mixed'; Instances = 500; Days = 14; Kind = 'mixed' },
    @{ Name = 'mature500-same'; Instances = 500; Days = 14; Kind = 'same' },
    @{ Name = 'mature500-distinct'; Instances = 500; Days = 14; Kind = 'distinct' }
)
if ($Spread -or ($Profile -and -not $Cases.Count)) { $matrix = @($matrix[3]) }
if ($Cases.Count) { $matrix = @($matrix | Where-Object { $_.Name -in $Cases }) }
$progressPath = Join-Path $OutputDirectory "$Phase-progress.txt"
foreach ($case in $matrix) {
    foreach ($mode in $Modes) {
        if ($mode -eq 'experiment' -and -not $AllWrapperCases -and
            $case.Name -notin @('initial500-mixed', 'mature500-mixed')) { continue }
        $suffix = if ($Spread) { '-spread' } else { '' }
        if ($Profile) { $suffix += '-profile' }
        $prefix = Join-Path $OutputDirectory "$Phase-$($case.Name)-$mode$suffix"
        $log = "$prefix.log"
        if (Test-Path -LiteralPath $log) {
            if ((Get-Content -LiteralPath $log -Tail 1) -match '^RESULT checks=\d+ failures=0$') {
                Add-Content -LiteralPath $progressPath -Value "$(Get-Date -Format o) PRESERVED $prefix"
                continue
            }
            throw "Refusing to overwrite incomplete/existing output: $log"
        }
        $arguments = @('--out', $prefix, '--instances', $case.Instances, '--days', $case.Days,
            '--case', $case.Kind, '--mode', $mode)
        if ($Spread) { $arguments += @('--schedule', 'spread') }
        if ($Profile) { $arguments += '--profile' }
        Add-Content -LiteralPath $progressPath -Value "$(Get-Date -Format o) START $prefix"
        & $Executable @arguments *> $log
        $result = $LASTEXITCODE
        Add-Content -LiteralPath $progressPath -Value "$(Get-Date -Format o) END exit=$result $prefix"
        if ($result -ne 0) { throw "Benchmark failed with exit $result : $log" }
    }
}
