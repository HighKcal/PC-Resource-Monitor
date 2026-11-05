<# Five-second passive diagnostic only. Does not inspect or manipulate windows.
   Main owns the execution slot. No product process is stopped by this script.
   Run summarize-uia.ps1 after the product has exited and CPU measurements end. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][int]$TargetPid,
    [Parameter(Mandatory=$true)][string]$Out,
    [ValidateRange(1,5)][int]$Seconds = 5
)
$ErrorActionPreference = 'Stop'
$outPath = [System.IO.Path]::GetFullPath($Out)
if (Test-Path -LiteralPath $outPath) { throw 'Use a new diagnostic output directory.' }
New-Item -ItemType Directory -Path $outPath | Out-Null
$sessionName = 'ResourceMonitor-UIA-' + [guid]::NewGuid().ToString('N')
$etlPath = Join-Path $outPath 'uia.etl'
$providerName = 'Microsoft-Windows-UIAutomationCore'
function Get-ProcessIdentitySnapshot {
    @(Get-Process | ForEach-Object {
        $creation = $null
        try { $creation = $_.StartTime.ToUniversalTime().ToFileTimeUtc().ToString() } catch {}
        [pscustomobject]@{ pid = $_.Id; image = $_.ProcessName; creation_filetime_100ns = $creation }
    })
}
$before = Get-ProcessIdentitySnapshot
$target = @($before | Where-Object { $_.pid -eq $TargetPid })
if ($target.Count -ne 1 -or !$target[0].creation_filetime_100ns) { throw 'Target identity is not readable.' }
$provider = Get-WinEvent -ListProvider $providerName
$provider.Events | Where-Object { $_.Id -in 509,510,517,518,519,520,523,524,544,545,546,547 } |
    ForEach-Object { [pscustomobject]@{ id=$_.Id; version=$_.Version; task=$_.Task.Name; opcode=$_.Opcode.Name; level=$_.Level.Value; template=$_.Template } } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outPath 'provider-schema.json') -Encoding utf8
$started = $false
$traceStart = $null
$watch = [Diagnostics.Stopwatch]::new()
$failure = $null
try {
    # Unique live session, no persisted collector, no existing session changes.
    # Fixed-size file bounds disk use; reaching the cap invalidates absence claims.
    & logman create trace $sessionName -p $providerName 0x4000000000010000 4 -o $etlPath -f bin -max 64 -bs 64 -nb 16 128 -ct perf -ets 2>&1 |
        Set-Content -LiteralPath (Join-Path $outPath 'start.log') -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "logman start failed: $LASTEXITCODE" }
    $started = $true
    $traceStart = [DateTime]::UtcNow.ToString('o')
    $watch.Start()
    Start-Sleep -Seconds $Seconds
} catch {
    $failure = $_.Exception.Message
} finally {
    if ($started) {
        & logman stop $sessionName -ets 2>&1 | Set-Content -LiteralPath (Join-Path $outPath 'stop.log') -Encoding utf8
        if ($LASTEXITCODE -ne 0) { $failure = "Stop failed; inspect owned session $sessionName. Previous failure: $failure" }
        $watch.Stop()
    }
    $after = Get-ProcessIdentitySnapshot
    $meta = [ordered]@{
        purpose='Passive UIA connection attribution diagnostic; not a CPU acceptance measurement'
        session=$sessionName; provider=$providerName; guid=$provider.Id.ToString()
        keywords='0x4000000000010000'; level=4; requested_seconds=$Seconds
        trace_start_utc=$traceStart; trace_end_utc=[DateTime]::UtcNow.ToString('o')
        observed_trace_wall_seconds=$watch.Elapsed.TotalSeconds
        target=$target[0]; processes_before=$before; processes_after=$after
        etl_max_megabytes=64; failure=$failure
        capture_script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    }
    $meta | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outPath 'capture.json') -Encoding utf8
}
if ($failure) { throw $failure }
Write-Output "Captured local diagnostic: $outPath. Stop measurement before running the offline parser."
