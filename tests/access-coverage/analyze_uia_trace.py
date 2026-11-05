"""Offline, streaming positive-only interpretation of the local UIA trace.

No process, window, ETW-session, policy, or privilege APIs are called. Truncated
or lost trace data cannot support absence, a complete five-second rate, or CPU
causation. XML rendered timestamps are retained verbatim and never used as UTC.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import ntpath
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

PROVIDER = "Microsoft-Windows-UIAutomationCore"
GUID = "820a42d8-38c4-465d-b64e-d7d56ea1d612"
HEADER_GUID = "9e814aad-3204-11d2-9a82-006008a86939"
MAX_PAIR_KEYS = 100000
MAX_METHODS = 10000


def local(tag):
    return tag.rsplit("}", 1)[-1]


def number(value):
    if value is None: return None
    value = value.strip()
    try: return int(value, 0)
    except ValueError:
        try: return int(value, 10)
        except ValueError: return None


def boolean(value):
    if value is None: return None
    return {"0": False, "1": True, "false": False, "true": True}.get(value.strip().lower())


def guid(value):
    return (value or "").strip("{}").lower()


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def read_text(path):
    raw = path.read_bytes()
    encodings = ["utf-16"] if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else ["utf-8-sig", "cp949"]
    for encoding in encodings:
        try: return raw.decode(encoding), encoding
        except UnicodeDecodeError: pass
    raise ValueError("Cannot decode evidence without replacement: " + str(path))


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, ensure_ascii=False)


def schema_metadata(path):
    text, encoding = read_text(path)
    root = ET.fromstring(text)
    if root.attrib.get("name") != PROVIDER or guid(root.attrib.get("guid")) != GUID:
        raise ValueError("Provider schema name/GUID does not match UIAutomationCore")
    tasks = {number(item.attrib.get("value")): item.attrib.get("name")
             for item in root.iter() if local(item.tag) == "task"}
    events = {}
    for item in root.iter():
        if local(item.tag) == "event":
            key = (number(item.attrib.get("value")), number(item.attrib.get("version")))
            if key in events: raise ValueError("Ambiguous duplicate schema event: " + str(key))
            events[key] = dict(task=number(item.attrib.get("task")), opcode=number(item.attrib.get("opcode")))
    expected = {517: (517, 1, "UiaProviderCallout"), 519: (519, 1, "MsaaProviderCallout"),
                544: (544, 0, "ClientConnection_RequestWritten"),
                545: (545, 0, "ServerConnection_RequestReceived")}
    for event_id, (task, opcode, name) in expected.items():
        if events.get((event_id, 0)) != dict(task=task, opcode=opcode) or tasks.get(task) != name:
            raise ValueError("Required schema event semantics differ: " + str(event_id))
    return events, dict(provider=PROVIDER, guid=GUID, source_encoding=encoding,
                        required_events={str(key): events[(key, 0)] for key in expected},
                        note="Native provider metadata identifies event/version/task/opcode. Named payload fields are checked in each decoded event; localized messages are not interpreted.")


def identities(snapshot):
    result = defaultdict(list)
    for item in snapshot["items"]: result[int(item["pid"])].append(item)
    return result


def checked_identity(pid, before, after):
    first, last = before.get(pid, []), after.get(pid, [])
    stable = (len(first) == len(last) == 1 and first[0].get("creation_filetime_100ns") is not None and
              int(first[0]["creation_filetime_100ns"]) > 0 and
              first[0]["creation_filetime_100ns"] == last[0].get("creation_filetime_100ns"))
    a, b = first[0] if len(first) == 1 else {}, last[0] if len(last) == 1 else {}
    path_a, path_b = a.get("path"), b.get("path")
    return dict(pid=pid, identity_verified=stable,
                creation_filetime_100ns=a.get("creation_filetime_100ns") if stable else None,
                identity_status="same PID+creation in both snapshots" if stable else "unresolved; do not connect by PID or name alone",
                handle_image_basename_before=ntpath.basename(path_a) if stable and path_a else None,
                handle_image_basename_after=ntpath.basename(path_b) if stable and path_b else None,
                handle_image_path_consistent=bool(stable and path_a and path_b and ntpath.normcase(path_a) == ntpath.normcase(path_b)),
                list_name_before=a.get("list_name"), list_name_after=b.get("list_name"),
                list_names_are_reference_only=True,
                before_matches=len(first), after_matches=len(last),
                before_query_errors={key: a.get(key) for key in ("open_error", "times_error", "path_error")},
                after_query_errors={key: b.get(key) for key in ("open_error", "times_error", "path_error")})


def event_parts(event):
    children = {local(item.tag): item for item in event}
    system = {local(item.tag): item for item in children.get("System", [])}
    provider = system.get("Provider"); execution = system.get("Execution"); clock = system.get("TimeCreated")
    def field(name):
        item = system.get(name)
        return number(item.text) if item is not None else None
    data = {}; duplicate_names = []
    for item in children.get("EventData", []):
        if local(item.tag) == "Data" and "Name" in item.attrib:
            key = item.attrib["Name"]
            if key in data: duplicate_names.append(key)
            data[key] = "".join(item.itertext()).strip()
    return dict(provider_name=provider.attrib.get("Name") if provider is not None else None,
                provider_guid=guid(provider.attrib.get("Guid")) if provider is not None else "",
                event_id=field("EventID"), version=field("Version"), task=field("Task"), opcode=field("Opcode"),
                pid=number(execution.attrib.get("ProcessID")) if execution is not None else None,
                tid=number(execution.attrib.get("ThreadID")) if execution is not None else None,
                rendered_timestamp=clock.attrib.get("SystemTime") if clock is not None else None,
                data=data, duplicate_data_names=duplicate_names, processing_error="ProcessingErrorData" in children)


def analyze(folder, output, sample_limit):
    files = {name: folder / name for name in ("events.xml", "provider-schema.xml", "processes-before.json",
              "processes-after.json", "capture.json", "tracerpt-summary.txt", "uia.etl")}
    for path in files.values():
        if not path.is_file(): raise FileNotFoundError(path)
    if output.exists(): raise FileExistsError("Use a fresh output directory: " + str(output))
    capture = json.loads(files["capture.json"].read_text(encoding="utf-8-sig"))
    before_raw = json.loads(files["processes-before.json"].read_text(encoding="utf-8-sig"))
    after_raw = json.loads(files["processes-after.json"].read_text(encoding="utf-8-sig"))
    before, after = identities(before_raw), identities(after_raw)
    target = capture["target"]["metadata"]
    target_pid, target_creation = int(target["pid"]), int(target["creation_filetime_100ns"])
    for key in ("target_before", "target_after"):
        state = capture[key]
        if not (state["alive"] and state["identity_matches"] and int(state["creation_filetime_100ns"]) == target_creation):
            raise ValueError("Target capture identity boundary is not verified: " + key)
    target_identity = checked_identity(target_pid, before, after)
    if not target_identity["identity_verified"] or int(target_identity["creation_filetime_100ns"]) != target_creation:
        raise ValueError("Target identity differs from the process snapshots")
    schema, schema_info = schema_metadata(files["provider-schema.xml"])
    input_hashes = {name: digest(path) for name, path in files.items()}
    summary_text, summary_encoding = read_text(files["tracerpt-summary.txt"])
    totals = {}
    for key, pattern in (("events_processed", r"Total\s+Events\s+Processed\s+(\d+)"),
                         ("events_lost", r"Total\s+Events\s+Lost\s+(\d+)"),
                         ("buffers_processed", r"Total\s+Buffers\s+Processed\s+(\d+)")):
        match = re.search(pattern, summary_text, re.IGNORECASE)
        totals[key] = int(match.group(1)) if match else None
    counts = Counter(); target_counts = Counter(); candidate_counts = Counter()
    method_counts = {517: Counter(), 519: Counter()}; call_starts = Counter()
    writes, receives = {}, {}
    headers = []; rendered_offsets = Counter()
    records = provider_records = mismatched_provider_records = unknown_versions = schema_mismatch = 0
    processing_errors = provider_processing_errors = duplicate_data_events = missing_connection_fields = missing_methods = 0
    pair_key_overflow = method_key_overflow = False
    first_rendered = last_rendered = None
    parse_failure = None
    try:
        stream = ET.iterparse(files["events.xml"], events=("start", "end"))
        _, root = next(stream)
        if local(root.tag) != "Events": raise ValueError("Unexpected trace XML root")
        for action, event in stream:
            if action != "end" or local(event.tag) != "Event": continue
            records += 1
            row = event_parts(event); data = row["data"]
            if row["processing_error"]: processing_errors += 1
            if row["provider_guid"] == HEADER_GUID and row["event_id"] == 0 and row["opcode"] == 0:
                headers.append({key: number(data.get(key)) for key in ("StartTime", "EndTime", "EventsLost", "BuffersLost", "MaxFileSize", "BuffersWritten", "BufferSize", "PerfFreq")})
            name_match = row["provider_name"] == PROVIDER
            guid_match = row["provider_guid"] == GUID
            if not (name_match and guid_match):
                if name_match or guid_match: mismatched_provider_records += 1
                root.clear(); continue
            provider_records += 1
            stamp = row["rendered_timestamp"]
            if first_rendered is None: first_rendered = stamp
            last_rendered = stamp
            offset = re.search(r"(Z|[+-]\d\d:\d\d)$", stamp or "")
            rendered_offsets[offset.group(1) if offset else "unparsed"] += 1
            event_id, version, pid = row["event_id"], row["version"], row["pid"]
            counts[(event_id, version)] += 1
            if pid == target_pid: target_counts[(event_id, version)] += 1
            if row["processing_error"]:
                provider_processing_errors += 1; root.clear(); continue
            if version != 0:
                unknown_versions += 1; root.clear(); continue
            expected = schema.get((event_id, version))
            if expected is None or expected != dict(task=row["task"], opcode=row["opcode"]):
                schema_mismatch += 1; root.clear(); continue
            if row["duplicate_data_names"]:
                duplicate_data_events += 1; root.clear(); continue
            if pid == target_pid and event_id in (517, 519):
                call_starts[event_id] += 1
                method = data.get("MethodName")
                if not method: missing_methods += 1
                elif method in method_counts[event_id] or len(method_counts[event_id]) < MAX_METHODS:
                    method_counts[event_id][method] += 1
                else: method_key_overflow = True
            if event_id not in (544, 545, 546, 547):
                root.clear(); continue
            peer, request = number(data.get("PeerProcessId")), number(data.get("RequestId"))
            cross = boolean(data.get("IsCrossMachine"))
            if pid != target_pid and peer != target_pid:
                root.clear(); continue
            scope = "target_header" if pid == target_pid else "numeric_peer_match_unverified"
            candidate_counts[(event_id, pid, peer, cross, scope)] += 1
            if not pid or not peer or request is None or (event_id in (544, 547) and cross is None):
                missing_connection_fields += 1; root.clear(); continue
            mapping = writes if event_id == 544 and cross is False else receives if event_id == 545 else None
            if mapping is not None:
                key = (pid, peer, request) if event_id == 544 else (peer, pid, request)
                if key in mapping:
                    mapping[key]["count"] += 1
                elif len(mapping) < MAX_PAIR_KEYS:
                    mapping[key] = dict(count=1, xml_record_index=records, header_pid=pid, header_tid=row["tid"],
                                        peer_pid=peer, request_id=request, event_id=event_id,
                                        rendered_timestamp=stamp, is_cross_machine=cross,
                                        is_cross_machine_raw=data.get("IsCrossMachine"), hresult_raw=data.get("HRESULT"))
                else: pair_key_overflow = True
            root.clear()  # Release completed Event elements from the parent, not only their children.
    except Exception as error:
        parse_failure = repr(error)
    header = headers[0] if len(headers) == 1 else None
    duration = ((header["EndTime"] - header["StartTime"]) / 1e7 if header and
                header["StartTime"] is not None and header["EndTime"] is not None and header["EndTime"] >= header["StartTime"] else None)
    max_bytes = int(capture["etl_max_megabytes"]) * 1024 * 1024
    trace_bytes = files["uia.etl"].stat().st_size
    output.mkdir(parents=True, exist_ok=False)
    pair_counts = Counter(); ambiguous_pairs = exported = 0
    peer_ids = set()
    with (output / "paired-requests.sample.jsonl").open("x", encoding="utf-8") as sample:
        for key in sorted(writes.keys() & receives.keys()):
            write, receive = writes[key], receives[key]
            if write["count"] != 1 or receive["count"] != 1:
                ambiguous_pairs += 1; continue
            client, server, request = key
            pair_counts[(client, server)] += 1
            peer_ids.update(pid for pid in (client, server) if pid != target_pid)
            if exported < sample_limit:
                sample.write(json.dumps(dict(client_pid=client, server_pid=server, request_id=request,
                    local_client_write=write, reverse_server_receive=receive), ensure_ascii=False) + "\n")
                exported += 1
    peers = {pid: checked_identity(pid, before, after) for pid in sorted(peer_ids)}
    pairs = []
    for (client, server), count in sorted(pair_counts.items()):
        other = server if client == target_pid else client
        checked = peers.get(other)
        pairs.append(dict(client_pid=client, server_pid=server, positive_paired_requests=count,
                          target_role="client" if client == target_pid else "server",
                          peer_identity_verified=checked["identity_verified"] if checked else False,
                          qualification="Observed local version-0 544(false)+reverse 545 with unique same RequestId. Identity/name attribution remains conditional on stable peer PID+creation."))
    counts_array = lambda source: [dict(event_id=key[0], version=key[1], count=value) for key, value in sorted(source.items(), key=lambda item: (str(item[0][0]), str(item[0][1])))]
    partial_reasons = []
    if trace_bytes >= max_bytes: partial_reasons.append("ETL reached the configured file cap")
    if not header: partial_reasons.append("Missing or ambiguous trace header")
    if header and (header["EventsLost"] is None or header["BuffersLost"] is None): partial_reasons.append("Loss counters are incomplete")
    if header and ((header["EventsLost"] or 0) > 0 or (header["BuffersLost"] or 0) > 0): partial_reasons.append("Trace reports lost events or buffers")
    stop_code = (capture.get("stop_command") or {}).get("exit_code")
    if stop_code != 0: partial_reasons.append("Owned-session stop did not succeed")
    if duration is None or duration < capture["requested_seconds"]: partial_reasons.append("Header interval is shorter than requested capture duration")
    if parse_failure: partial_reasons.append("XML parse failed before EOF")
    if totals["events_processed"] is not None and totals["events_processed"] != records:
        partial_reasons.append("Native tracerpt event total differs from streamed XML event count")
    if provider_processing_errors: partial_reasons.append("UIAutomationCore events contain processing errors and were excluded from interpretation")
    if unknown_versions or schema_mismatch or missing_connection_fields or duplicate_data_events:
        partial_reasons.append("Some provider events could not be interpreted under the checked schema")
    if pair_key_overflow or method_key_overflow: partial_reasons.append("Bounded aggregation capacity reached")
    report = dict(
        diagnostic_only=True, positive_observations_only=True, partial_capture=bool(partial_reasons),
        partial_reasons=partial_reasons,
        forbidden_conclusions=["absence of other callers or activity", "complete requested-five-second counts or rates", "CPU causation or attributable CPU percentage", "equating connection events with desktop tree scans"],
        target=dict(pid=target_pid, creation_filetime_100ns=target_creation, exe_sha256=target["exe_sha256"],
                    actual_token=target["child_token"], identity=target_identity),
        capture_bounds=dict(requested_seconds=capture["requested_seconds"], supervisor_observed_seconds=capture["observed_trace_wall_seconds"],
                            supervisor_start_utc=capture["trace_start_utc"], supervisor_stop_utc=capture["trace_end_utc"],
                            header_count=len(headers), header=header, header_interval_seconds=duration,
                            interval_basis="(header EndTime - StartTime)/10,000,000; no rendered timestamp conversion",
                            rendered_first_in_file=first_rendered, rendered_last_in_file=last_rendered,
                            rendered_offsets=dict(rendered_offsets), rendered_timestamp_warning="Raw strings only. The observed +08:59 rendering is not used to infer absolute UTC or durations."),
        loss_and_cap=dict(etl_bytes=trace_bytes, configured_cap_bytes=max_bytes, at_or_above_cap=trace_bytes >= max_bytes,
                          events_lost=header["EventsLost"] if header else None, buffers_lost=header["BuffersLost"] if header else None,
                          native_tracerpt_totals=totals, summary_source_encoding=summary_encoding,
                          native_total_matches_parsed=totals["events_processed"] == records if totals["events_processed"] is not None else None,
                          stop_exit_code=stop_code, capture_failure=capture.get("failure")),
        schema=schema_info,
        parsing=dict(records_read=records, provider_records=provider_records, mismatched_name_or_guid=mismatched_provider_records,
                     unknown_versions=unknown_versions, schema_mismatches=schema_mismatch,
                     processing_error_records=processing_errors, provider_processing_error_records=provider_processing_errors,
                     duplicate_payload_names=duplicate_data_events,
                     missing_connection_fields=missing_connection_fields, missing_method_names=missing_methods,
                     parse_failure=parse_failure, pair_key_cap=MAX_PAIR_KEYS, pair_key_cap_reached=pair_key_overflow,
                     method_key_cap_reached=method_key_overflow, xml_elements_released_with_root_clear=True),
        provider_event_counts=counts_array(counts), target_header_event_counts=counts_array(target_counts),
        target_provider_calls=[dict(event_id=eid, task="UiaProviderCallout" if eid == 517 else "MsaaProviderCallout",
                              phase="Start only", observed_starts=call_starts[eid], starts_with_method_name=sum(method_counts[eid].values()),
                              methods=[dict(method=name, count=count) for name, count in method_counts[eid].most_common()]) for eid in (517, 519)],
        connection_candidates=[dict(event_id=key[0], header_pid=key[1], peer_pid=key[2], is_cross_machine=key[3], scope=key[4], count=value)
                               for key, value in candidate_counts.items()],
        local_request_pairing=dict(pairs=pairs, positive_paired_requests=sum(pair_counts.values()),
                                   ambiguous_repeated_keys=ambiguous_pairs, unmatched_local_write_keys=len(writes.keys()-receives.keys()),
                                   unmatched_receive_keys=len(receives.keys()-writes.keys()), sample_rows=exported,
                                   sample_limit=sample_limit, rule="Each key=(client PID,server PID,RequestId) occurs once as local 544(IsCrossMachine=false) and once as reversed 545. Other records are not directional proof."),
        checked_peer_identities=list(peers.values()),
        process_snapshot_quality=dict(before_error=before_raw["enumeration_error"], after_error=after_raw["enumeration_error"],
                                      before_started_utc=before_raw["started_utc"], before_finished_utc=before_raw["finished_utc"],
                                      after_started_utc=after_raw["started_utc"], after_finished_utc=after_raw["finished_utc"]),
        evidence=[dict(path=str(files[name]), sha256=value) for name, value in input_hashes.items()],
        parser_sha256=digest(Path(__file__).resolve()),
    )
    changed = [name for name, path in files.items() if digest(path) != input_hashes[name]]
    report["inputs_unchanged"] = not changed
    report["changed_inputs"] = changed
    write_json(output / "report.json", report)
    print(json.dumps(dict(output=str(output), records_read=records, positive_paired_requests=sum(pair_counts.values()),
                          target_provider_starts={str(eid): call_starts[eid] for eid in (517, 519)},
                          partial_capture=bool(partial_reasons), header_interval_seconds=duration, errors=partial_reasons)))
    if parse_failure or changed: return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--sample-limit", type=int, default=5000)
    args = parser.parse_args()
    if not 0 <= args.sample_limit <= 5000: parser.error("--sample-limit must be 0..5000")
    return analyze(args.trace.resolve(strict=True), args.out.resolve(), args.sample_limit)


if __name__ == "__main__":
    sys.exit(main())
