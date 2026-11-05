param(
    [string]$Output = 'build-review-native-ui-final/source-target.json'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$paths = @(
    'gui/monitor_gui.c',
    'gui/native_ui.h',
    'gui/native_demo.h',
    'gui/native_detail.h',
    'gui/native_projection.h',
    'scripts/build-monitor.cmd',
    'include/monitor.h',
    'include/resource_collector.h',
    'include/process_grouping.h',
    'include/program_groups.h',
    'grouping/process_grouping.c',
    'grouping/process_grouping_windows.c',
    'grouping/program_groups.c',
    'tests/review-native-ui/test_native_ui_review.c',
    'tests/review-native-ui/build-review.cmd'
)
$result = foreach ($relative in $paths) {
    $absolute = Join-Path $repo $relative
    [pscustomobject]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash }
}
$destination = Join-Path $repo $Output
$directory = Split-Path -Parent $destination
if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory | Out-Null }
$result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $destination -Encoding utf8
Write-Output $destination
