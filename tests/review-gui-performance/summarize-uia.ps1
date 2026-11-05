<# Offline only: inspect the ETL after all performance measurement has stopped.
   Summaries omit object references, HWNDs, UI strings and full process paths.
   Trace completeness still requires inspecting tracerpt-summary.txt loss counts. #>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Out,
      [ValidateRange(1,100000)][int]$MaxRequestRows = 5000)
$ErrorActionPreference = 'Stop'
$outPath = [System.IO.Path]::GetFullPath($Out)
$meta = Get-Content -LiteralPath (Join-Path $outPath 'capture.json') -Raw | ConvertFrom-Json
if ($meta.failure) { throw "Capture failed: $($meta.failure)" }
$etlPath = Join-Path $outPath 'uia.etl'
$summaryPath = Join-Path $outPath 'tracerpt-summary.txt'
if (Test-Path -LiteralPath (Join-Path $outPath 'summary.json')) { throw 'Summary already exists; preserve it.' }
& tracerpt $etlPath -o NUL -summary $summaryPath -y 2>&1 |
    Set-Content -LiteralPath (Join-Path $outPath 'tracerpt.log') -Encoding utf8
$tracerptExit = $LASTEXITCODE
$counts = @{}
$peerIds = [Collections.Generic.HashSet[int]]::new()
$requestRows = 0L
$targetRecords = 0L
$recordsRead = 0L
$unknownVersions = 0L
$parseFailure = $null
$rowsPath = Join-Path $outPath 'requests.sample.jsonl'
$writer = [IO.StreamWriter]::new($rowsPath, $false, [Text.UTF8Encoding]::new($false))
try {
    # Stream the ETL. No full XML dump and no UI Automation COM client are used.
    Get-WinEvent -Path $etlPath -Oldest -ErrorAction Stop | ForEach-Object {
        ++$recordsRead
        if ($_.ProviderName -eq 'Microsoft-Windows-UIAutomationCore') {
            $eventId = [int]$_.Id
            $headerPid = [int]$_.ProcessId
            if ($eventId -in 544,545,546,547 -or ($headerPid -eq [int]$meta.target.pid -and $eventId -in 517,519,523)) {
                if ($_.Version -ne 0) { ++$unknownVersions }
                else {
                    $values = @{}
                    $xml = [xml]$_.ToXml()
                    foreach ($datum in $xml.Event.EventData.Data) { $values[[string]$datum.Name] = [string]$datum.'#text' }
                    $peer = 0
                    if ($values.ContainsKey('PeerProcessId')) { $peer = [int]$values.PeerProcessId }
                    if ($headerPid -eq [int]$meta.target.pid -or $peer -eq [int]$meta.target.pid) {
                        ++$targetRecords
                        $key = "$eventId/$headerPid/$peer"
                        if (!$counts.ContainsKey($key)) { $counts[$key] = 0L }
                        ++$counts[$key]
                        if ($peer -and $peer -ne [int]$meta.target.pid) { $null = $peerIds.Add($peer) }
                        if ($headerPid -ne [int]$meta.target.pid) { $null = $peerIds.Add($headerPid) }
                        if ($eventId -in 544,545,546,547) {
                            ++$requestRows
                            if ($requestRows -le $MaxRequestRows) {
                                $row = [ordered]@{ event_id=$eventId; version=0; utc=$_.TimeCreated.ToUniversalTime().ToString('o'); header_pid=$headerPid; header_tid=$_.ThreadId; peer_pid=$peer; request_id=$values.RequestId }
                                $writer.WriteLine(($row | ConvertTo-Json -Compress))
                            }
                        }
                    }
                }
            }
        }
        $_.Dispose()
    }
} catch { $parseFailure = $_.Exception.Message }
finally { $writer.Dispose() }
$identities = @($peerIds | ForEach-Object {
    $peerId = $_
    $before = @($meta.processes_before | Where-Object { $_.pid -eq $peerId })
    $after = @($meta.processes_after | Where-Object { $_.pid -eq $peerId })
    $status = 'not present at both snapshots; identity unresolved'
    if ($before.Count -eq 1 -and $after.Count -eq 1 -and $before[0].creation_filetime_100ns -and $before[0].creation_filetime_100ns -eq $after[0].creation_filetime_100ns) { $status = 'same PID and creation at both snapshots' }
    [pscustomobject]@{ pid=$peerId; before=$before; after=$after; identity_status=$status }
})
$entries = @($counts.GetEnumerator() | Sort-Object Name | ForEach-Object {
    $parts = $_.Name.Split('/')
    [pscustomobject]@{ event_id=[int]$parts[0]; header_pid=[int]$parts[1]; peer_pid=[int]$parts[2]; count=$_.Value }
})
$result = [ordered]@{
    diagnostic_only=$true; target=$meta.target; provider='Microsoft-Windows-UIAutomationCore'
    records_read=$recordsRead; target_related_records=$targetRecords; unknown_versions=$unknownVersions
    request_records_total=$requestRows; request_rows_exported=[Math]::Min($requestRows,$MaxRequestRows)
    counts=$entries; peer_identities=$identities; parse_failure=$parseFailure; tracerpt_exit_code=$tracerptExit
    loss_status='UNVERIFIED: inspect tracerpt-summary.txt EventsLost and BuffersLost before any absence claim'
    trace_size_bytes=(Get-Item -LiteralPath $etlPath).Length
    trace_near_file_cap=((Get-Item -LiteralPath $etlPath).Length -ge 60MB)
    etl_sha256=(Get-FileHash -LiteralPath $etlPath -Algorithm SHA256).Hash
    parser_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    interpretation='544 emitter is client-side connection writer; peer is server. 545 emitter is server receiver; peer is client. Match RequestId and opposite PID pair in sampled rows. HeaderPID alone is not always requester. These are connection records, not tree scans.'
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outPath 'summary.json') -Encoding utf8
Write-Output "Offline summary saved: $outPath. Loss counts remain to be reviewed explicitly."
if ($parseFailure -or $tracerptExit -ne 0 -or $unknownVersions) { exit 1 }
