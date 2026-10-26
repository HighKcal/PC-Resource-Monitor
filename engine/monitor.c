#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "monitor_internal.h"
#include "storage_internal.h"

#define TICKS MONITOR_TICKS_PER_SECOND
#define DAY MONITOR_DAY_TICKS
#include "baseline.h"
static const unsigned metric_bits[4] = { RESOURCE_VALID_CPU, RESOURCE_VALID_MEMORY,
    RESOURCE_VALID_IO_READ, RESOURCE_VALID_IO_WRITE };

static char *copy_string(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1; p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static void unavailable(MonitorMetricReport *r, const char *reason)
{
    memset(r, 0, sizeof(*r)); r->state = MONITOR_UNAVAILABLE;
    r->current = r->median = r->mad = r->threshold = NAN;
    snprintf(r->reason, sizeof(r->reason), "%s", reason);
}
const char *monitor_state_name(MonitorState s)
{
    static const char *const names[] = { "unavailable", "learning", "normal", "attention", "candidate" };
    return s >= MONITOR_UNAVAILABLE && s <= MONITOR_CANDIDATE ? names[s] : "unknown";
}
const char *monitor_metric_name(unsigned metric)
{
    static const char *const names[] = { "CPU %", "Working set bytes", "Read B/s", "Write B/s" };
    return metric < 4 ? names[metric] : "unknown";
}
void monitor_snapshot_free(MonitorSnapshot *s)
{
    size_t i;
    if (!s) return;
    for (i = 0; i < s->count; ++i) { free(s->rows[i].name); free(s->rows[i].path); }
    free(s->rows); free(s);
}
static void free_instances(MonitorInstance *p, size_t count)
{
    size_t i;
    if (!p) return;
    for (i = 0; i < count; ++i) free(p[i].path);
    free(p);
}
void monitor_reset(MonitorEngine *e)
{
    if (!e) return;
    free_instances(e->instances, e->count); e->instances = NULL; e->count = 0;
}
static void reset_window(MonitorInstance *p, uint64_t anchor, int reset_streak)
{
    unsigned m;
    p->start = p->last_utc = anchor; p->duration = 0;
    memset(p->sum, 0, sizeof(p->sum)); memset(p->covered, 0, sizeof(p->covered));
    memset(p->samples, 0, sizeof(p->samples));
    if (reset_streak) {
        memset(p->streak, 0, sizeof(p->streak));
        for (m = 0; m < 4; ++m) {
            unavailable(&p->report[m], "collecting complete 60-75s window");
            p->report[m].state = p->program_id ? MONITOR_LEARNING : MONITOR_UNAVAILABLE;
        }
    }
}
static int save_window(MonitorEngine *e, MonitorInstance *p, sqlite3_int64 run, sqlite3_int64 cutoff)
{
    sqlite3_stmt *s = NULL;
    sqlite3_int64 id;
    unsigned m;
    if (!monitor_db_prepare(e, "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,?,?,?,?,?,1)", &s)) return 0;
    sqlite3_bind_int64(s, 1, run); sqlite3_bind_int64(s, 2, p->program_id);
    sqlite3_bind_int64(s, 3, p->pid); sqlite3_bind_int64(s, 4, (sqlite3_int64)p->creation);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)p->start); sqlite3_bind_int64(s, 6, (sqlite3_int64)p->last_utc);
    sqlite3_bind_double(s, 7, p->duration);
    if (!monitor_db_done(e, s)) { sqlite3_finalize(s); return 0; }
    sqlite3_finalize(s); s = NULL; id = sqlite3_last_insert_rowid(e->db);
    if (!monitor_db_prepare(e, "INSERT INTO aggregate_metric VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &s)) return 0;
    for (m = 0; m < 4; ++m) {
        MonitorMetricReport *r = &p->report[m];
        int quality = m == MONITOR_RAM ? p->samples[m] >= 15 :
            p->covered[m] >= p->duration * 0.75 && p->covered[m] <= p->duration * 1.25;
        unavailable(r, "insufficient metric coverage or valid sample count");
        r->covered_seconds = p->covered[m]; r->valid_samples = p->samples[m]; r->window_end_utc_100ns = p->last_utc;
        r->window_start_utc_100ns = p->start; r->window_duration_seconds = p->duration;
        if (p->samples[m] && (m == MONITOR_RAM || p->covered[m] > 0))
            r->current = p->sum[m] / (m == MONITOR_RAM ? p->samples[m] : p->covered[m]);
        if (quality && isfinite(r->current)) {
            if (!monitor_baseline(e, p, m, cutoff, r)) { sqlite3_finalize(s); return 0; }
        } else p->streak[m] = 0;
        sqlite3_bind_int64(s, 1, id); sqlite3_bind_int(s, 2, (int)m);
        monitor_db_bind_value(s, 3, isfinite(r->current), r->current); sqlite3_bind_double(s, 4, r->covered_seconds);
        sqlite3_bind_int(s, 5, (int)r->valid_samples); sqlite3_bind_int(s, 6, r->baseline_eligible);
        sqlite3_bind_int(s, 7, r->state); sqlite3_bind_text(s, 8, r->reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 9, (int)r->count); sqlite3_bind_int(s, 10, (int)r->days); sqlite3_bind_int(s, 11, (int)r->minutes);
        monitor_db_bind_value(s, 12, isfinite(r->median), r->median); monitor_db_bind_value(s, 13, isfinite(r->mad), r->mad);
        monitor_db_bind_value(s, 14, isfinite(r->threshold), r->threshold); sqlite3_bind_int(s, 15, (int)r->streak);
        if (!monitor_db_done(e, s)) { sqlite3_finalize(s); return 0; }
    }
    sqlite3_finalize(s); reset_window(p, p->last_utc, 0); return 1;
}
static int advance_window(MonitorEngine *e, MonitorInstance *p, const ResourceData *r,
    sqlite3_int64 run, sqlite3_int64 cutoff, size_t *completed)
{
    double values[4], intervals[4];
    unsigned m;
    int observation_ok = (r->valid_fields & RESOURCE_VALID_OBSERVATION_INTERVAL) &&
        isfinite(r->observation_interval_seconds) && r->observation_interval_seconds > 0 && r->observation_interval_seconds <= 15;
    if (!p->program_id || !(r->valid_fields & RESOURCE_VALID_IDENTITY)) {
        reset_window(p, r->measured_at_utc_100ns, 1); return 1;
    }
    if (!p->start || !observation_ok || r->measured_at_utc_100ns <= p->last_utc ||
        p->duration + r->observation_interval_seconds > 75) {
        reset_window(p, r->measured_at_utc_100ns, 1); return 1;
    }
    values[0] = r->cpu_percent; values[1] = (double)r->working_set_bytes;
    values[2] = r->io_read_bytes_per_second; values[3] = r->io_write_bytes_per_second;
    intervals[0] = r->cpu_interval_seconds; intervals[1] = r->observation_interval_seconds;
    intervals[2] = intervals[3] = r->io_interval_seconds;
    p->duration += r->observation_interval_seconds; p->last_utc = r->measured_at_utc_100ns;
    for (m = 0; m < 4; ++m) {
        if ((r->valid_fields & metric_bits[m]) && isfinite(values[m]) && values[m] >= 0 &&
            isfinite(intervals[m]) && intervals[m] > 0 && intervals[m] <= 15) {
            p->sum[m] += values[m] * (m == MONITOR_RAM ? 1 : intervals[m]);
            p->covered[m] += intervals[m]; ++p->samples[m];
        } else {
            p->streak[m] = 0; p->report[m].streak = 0;
            p->report[m].state = MONITOR_UNAVAILABLE;
            snprintf(p->report[m].reason, sizeof(p->report[m].reason), "latest metric missing; persistence reset");
        }
    }
    if (p->duration >= 60) {
        if (!save_window(e, p, run, cutoff)) return 0;
        ++*completed;
    }
    return 1;
}
static int valid_input(const ResourceBatch *batch)
{
    size_t i;
    if (!batch || (batch->count && !batch->items) || batch->count > 1000000 ||
        batch->started_at_utc_100ns > INT64_MAX || batch->finished_at_utc_100ns > INT64_MAX ||
        batch->foreground_observed_at_utc_100ns > INT64_MAX) return 0;
    for (i = 0; i < batch->count; ++i) {
        const ResourceData *r = &batch->items[i];
        if (r->measured_at_utc_100ns > INT64_MAX ||
            ((r->valid_fields & RESOURCE_VALID_IDENTITY) && (!r->creation_time_utc_100ns || r->creation_time_utc_100ns > INT64_MAX)) ||
            ((r->valid_fields & RESOURCE_VALID_MEMORY) && r->working_set_bytes > INT64_MAX)) return 0;
    }
    return 1;
}
int monitor_ingest(MonitorEngine *e, const ResourceBatch *batch, const char *const *paths, MonitorSnapshot **out)
{
    MonitorSnapshot *snapshot = NULL;
    MonitorInstance *next = NULL;
    sqlite3_stmt *s = NULL;
    sqlite3_int64 run, cutoff;
    size_t i, j;
    LARGE_INTEGER begin, end, frequency;
    int ok;
    if (out) *out = NULL;
    if (!e || !out) return 0;
    if (!valid_input(batch) || (paths && !e->synthetic)) {
        snprintf(e->error, sizeof(e->error), "Invalid batch or real DB fixture injection"); monitor_reset(e); return 0;
    }
    e->error[0] = 0;
    QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&frequency);
    snapshot = calloc(1, sizeof(*snapshot));
    next = calloc(batch->count ? batch->count : 1, sizeof(*next));
    if (!snapshot || !next) goto memory_failed;
    snapshot->rows = calloc(batch->count ? batch->count : 1, sizeof(*snapshot->rows));
    if (!snapshot->rows) goto memory_failed;
    snapshot->count = batch->count; snapshot->synthetic = e->synthetic;
    snapshot->measured_utc_100ns = batch->finished_at_utc_100ns;
    snapshot->foreground_observed_utc_100ns = batch->foreground_observed_at_utc_100ns;
    snapshot->foreground_pid = batch->foreground_pid;
    if (!monitor_db_execute(e, "BEGIN IMMEDIATE")) goto failed;
    cutoff = monitor_db_scalar(e, "SELECT COALESCE(max(id),0) FROM aggregate_window", &ok); if (!ok) goto failed;
    if (!monitor_db_prepare(e, "INSERT INTO collection_run(session_id,started,finished,status,row_count,cpu_count,foreground_utc,foreground_pid) VALUES(?,?,?,0,?,?,?,?)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, e->session); sqlite3_bind_int64(s, 2, (sqlite3_int64)batch->started_at_utc_100ns);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)batch->finished_at_utc_100ns);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)batch->count); sqlite3_bind_int64(s, 5, batch->logical_processor_count);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)batch->foreground_observed_at_utc_100ns);
    if (batch->foreground_pid) sqlite3_bind_int64(s, 7, batch->foreground_pid);
    if (!monitor_db_done(e, s)) goto failed;
    sqlite3_finalize(s); s = NULL; run = sqlite3_last_insert_rowid(e->db);
    for (i = 0; i < batch->count; ++i) {
        const ResourceData *r = &batch->items[i];
        MonitorInstance *p = &next[i];
        MonitorRow *row = &snapshot->rows[i];
        unsigned m;
        p->pid = r->pid; p->creation = r->creation_time_utc_100ns;
        if (r->valid_fields & RESOURCE_VALID_IDENTITY) {
            for (j = 0; j < e->count; ++j) if (e->instances[j].pid == r->pid && e->instances[j].creation == r->creation_time_utc_100ns) break;
            if (j < e->count) {
                *p = e->instances[j]; p->path = copy_string(e->instances[j].path);
                if (e->instances[j].path && !p->path) goto memory_failed;
            }
            if (!p->path) {
                p->path = paths ? copy_string(paths[i]) : monitor_query_path(r->pid, r->creation_time_utc_100ns, &p->path_error);
                if (paths && paths[i] && !p->path) goto memory_failed;
                if (p->path) {
                    p->path_error = 0; p->program_id = monitor_db_program_id(e, p->path, r->process_name_utf8, r->measured_at_utc_100ns);
                    if (!p->program_id) goto failed;
                } else if (paths) p->path_error = ERROR_ACCESS_DENIED;
            }
        } else p->path_error = ERROR_INVALID_DATA;
        if (!p->start) for (m = 0; m < 4; ++m) unavailable(&p->report[m], "identity/path unavailable");
        if (!monitor_db_save_raw(e, run, r, p) || !advance_window(e, p, r, run, cutoff, &snapshot->completed_windows)) goto failed;
        row->raw = *r; row->name = copy_string(r->process_name_utf8 ? r->process_name_utf8 : "(unavailable)");
        row->path = copy_string(p->path); row->raw.process_name_utf8 = row->name;
        if (!row->name || (p->path && !row->path)) goto memory_failed;
        row->program_id = p->program_id; row->path_error = p->path_error;
        memcpy(row->metric, p->report, sizeof(row->metric));
        if (!monitor_db_get_feedback(e, row, batch->finished_at_utc_100ns)) goto failed;
    }
    if (!monitor_db_prepare(e, "UPDATE program SET last_seen=max(last_seen,?) WHERE id IN(SELECT program_id FROM raw WHERE run_id=? AND program_id IS NOT NULL)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)batch->finished_at_utc_100ns); sqlite3_bind_int64(s, 2, run);
    if (!monitor_db_done(e, s)) goto failed;
    sqlite3_finalize(s); s = NULL;
    if (!monitor_db_trim(e, batch->finished_at_utc_100ns) || !monitor_db_execute(e, "COMMIT")) goto failed;
    free_instances(e->instances, e->count); e->instances = next; e->count = batch->count; next = NULL;
    snapshot->sequence = (uint64_t)++e->sequence;
    QueryPerformanceCounter(&end); snapshot->store_ms = (double)(end.QuadPart - begin.QuadPart) * 1000 / (double)frequency.QuadPart;
    snprintf(snapshot->status, sizeof(snapshot->status), "%srecorded %zu rows; %zu completed windows; local DB; raw 1h / summary 28d",
        e->synthetic ? "SYNTHETIC - " : "", snapshot->count, snapshot->completed_windows);
    *out = snapshot; return 1;
memory_failed:
    snprintf(e->error, sizeof(e->error), "Out of memory; window/streak state discarded");
failed:
    sqlite3_finalize(s);
    sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    free_instances(next, batch->count); monitor_snapshot_free(snapshot); monitor_reset(e); return 0;
}
