/* 회차별 저장 트랜잭션과 인스턴스 관측창·Baseline을 조율한다.
 * DB COMMIT 뒤에만 새 상태와 독립 snapshot을 공개한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "monitor_engine_private.h"
#include "monitor_source_store.h"

#define TICKS MONITOR_TICKS_PER_SECOND
#define DAY MONITOR_DAY_TICKS
#include "baseline.h"
static char *copy_string(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
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
    for (i = 0; i < s->count; ++i) {
        free(s->rows[i].name);
        free(s->rows[i].path);
    }
    free(s->rows);
    free(s);
}

static void free_instances(MonitorInstance *p, size_t count)
{
    size_t i;
    if (!p) return;
    for (i = 0; i < count; ++i) free(p[i].path);
    free(p);
}


/* 포화 시 발급을 중단하는 CAS 카운터. 이 프로세스에서 cookie를 재사용하지 않는다. */
__declspec(align(8)) static volatile LONG64 engine_cookie_counter;
static uint64_t new_engine_cookie(void)
{
    LONG64 previous;
    for (;;) {
        previous = InterlockedCompareExchange64(&engine_cookie_counter, 0, 0);
        if (previous == LLONG_MAX) return 0;
        if (InterlockedCompareExchange64(&engine_cookie_counter, previous + 1, previous) == previous)
            return (uint64_t)(previous + 1);
    }
}
static void invalidate_commit(MonitorEngine *e)
{
    e->committed_snapshot = NULL;
    e->committed_rows = NULL;
    memset(&e->committed_ref, 0, sizeof(e->committed_ref));
    if (e->mutation_epoch == UINT64_MAX) e->refs_disabled = 1;
    else ++e->mutation_epoch;
}
static int store_result(MonitorEngine *e, int ok)
{
    if (!ok) snprintf(e->error, sizeof(e->error), "%s", monitor_store_error(e->store));
    return ok;
}
const char *monitor_error(const MonitorEngine *e) { return e ? e->error : "engine unavailable"; }
int monitor_open(const char *path, int synthetic, MonitorEngine **out, char *error, size_t size)
{
    MonitorEngine *e;
    if (out) *out = NULL;
    if (error && size) error[0] = 0;
    if (!out || !path || !path[0] || (synthetic != 0 && synthetic != 1)) return 0;
    e = calloc(1, sizeof(*e));
    if (!e) return 0;
    e->synthetic = synthetic;
    e->cookie = new_engine_cookie();
    if (!e->cookie) {
        if (error && size) snprintf(error, size, "Engine identifier exhausted");
        free(e);
        return 0;
    }
    if (!monitor_store_open(path, synthetic, &e->store, error, size)) { free(e); return 0; }
    e->session = monitor_store_session(e->store);
    *out = e;
    return 1;
}
void monitor_close(MonitorEngine *e)
{
    if (!e) return;
    monitor_reset(e);
    monitor_store_close(e->store);
    free(e);
}
int monitor_storage_stats(MonitorEngine *e, MonitorStorageStats *out)
{
    if (!e || !out) return 0;
    return store_result(e, monitor_store_stats(e->store, out));
}
int monitor_record_failure(MonitorEngine *e, uint64_t utc, unsigned status)
{
    if (!e) return 0;
    invalidate_commit(e);
    if (utc > INT64_MAX) return 0;
    monitor_reset(e);
    return store_result(e, monitor_store_record_failure(e->store, utc, status));
}
int monitor_feedback(MonitorEngine *e, int64_t program, MonitorFeedback kind, uint64_t now)
{
    uint64_t duration = kind == MONITOR_FEEDBACK_IGNORE ? 3600 * TICKS : 86400 * TICKS;
    if (!e) return 0;
    invalidate_commit(e);
    if (program <= 0 || kind < MONITOR_FEEDBACK_CANCEL || kind > MONITOR_FEEDBACK_EXPECTED || now > INT64_MAX - duration) return 0;
    return store_result(e, monitor_store_feedback(e->store, program, kind, now));
}

void monitor_reset(MonitorEngine *e)
{
    if (!e) return;
    invalidate_commit(e);
    baseline_history_clear(&e->history);
    free_instances(e->instances, e->count);
    e->instances = NULL;
    e->count = 0;
}

static void reset_window(MonitorInstance *p, uint64_t anchor, int reset_streak)
{
    unsigned m;
    monitor_window_reset(&p->window, anchor);
    if (reset_streak) {
        memset(p->streak, 0, sizeof(p->streak));
        for (m = 0; m < 4; ++m) {
            monitor_window_unavailable(&p->report[m], "collecting complete 60-75s window");
            p->report[m].state = p->program_id ? MONITOR_LEARNING : MONITOR_UNAVAILABLE;
        }
    }
}
static int prepare_window_metric_report(MonitorEngine *engine, MonitorInstance *instance,
    unsigned metric)
{
    MonitorMetricReport *report = &instance->report[metric];
    if (monitor_window_metric(&instance->window, metric, report)) {
        BaselineInput input;
        unsigned next_streak;
        input.program_id = instance->program_id; input.exact_start = instance->window.start;
        input.metric = metric; input.prior_streak = instance->streak[metric];
        if (!monitor_baseline(&engine->history, &input, report, &next_streak)) return store_result(engine, 0);
        instance->streak[metric] = next_streak;
    } else instance->streak[metric] = 0;
    return 1;
}
static int save_window(MonitorEngine *e, MonitorInstance *p, int64_t run)
{
    MonitorWindowHeader header;
    MonitorWindowWrite scope = {0};
    unsigned m;
    header.run_id = run; header.program_id = p->program_id; header.pid = p->pid;
    header.creation = p->creation; header.start = p->window.start;
    header.end = p->window.last_utc; header.duration = p->window.duration;
    if (!store_result(e, monitor_store_begin_window(e->store, &header, &scope))) return 0;
    /* 창 헤더 뒤 metric별 품질·과거 비교·저장을 마치고 임시 statement를 해제한다. */
    for (m = 0; m < 4; ++m) {
        if (!prepare_window_metric_report(e, p, m) ||
            !store_result(e, monitor_store_write_metric(e->store, &scope, m, &p->report[m]))) {
            monitor_store_end_window(&scope);
            return 0;
        }
    }
    monitor_store_end_window(&scope);
    reset_window(p, p->window.last_utc, 0);
    return 1;
}
static int advance_window(MonitorEngine *e, MonitorInstance *p, const ResourceData *r,
    int64_t run, size_t *completed)
{
    MonitorWindowStep step = monitor_window_observe(&p->window, r, p->program_id != 0);
    unsigned m;
    if (step.reset) { reset_window(p, r->measured_at_utc_100ns, 1); return 1; }
    for (m = 0; m < 4; ++m) if (step.missing & (1u << m)) {
        p->streak[m] = 0;
        p->report[m].streak = 0;
        p->report[m].state = MONITOR_UNAVAILABLE;
        snprintf(p->report[m].reason, sizeof(p->report[m].reason), "latest metric missing; persistence reset");
    }
    if (step.complete) {
        if (!save_window(e, p, run)) return 0;
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

typedef enum InstancePrepareResult {
    INSTANCE_PREPARE_DATABASE_FAILED = 0,
    INSTANCE_PREPARE_OK = 1,
    INSTANCE_PREPARE_MEMORY_FAILED = -1
} InstancePrepareResult;

/* 다음 상태 배열이 복사된 경로를 소유하며 현재 engine 배열은 변경하지 않는다. */
static InstancePrepareResult prepare_instance(MonitorEngine *engine, MonitorInstance *instance,
    const ResourceData *raw, const char *const *paths, size_t index)
{
    size_t previous_index;
    unsigned metric;
    instance->pid = raw->pid;
    instance->creation = raw->creation_time_utc_100ns;
    if (raw->valid_fields & RESOURCE_VALID_IDENTITY) {
        for (previous_index = 0; previous_index < engine->count; ++previous_index) {
            if (engine->instances[previous_index].pid == raw->pid &&
                engine->instances[previous_index].creation == raw->creation_time_utc_100ns) {
                break;
            }
        }
        if (previous_index < engine->count) {
            *instance = engine->instances[previous_index];
            instance->path = copy_string(engine->instances[previous_index].path);
            if (engine->instances[previous_index].path && !instance->path) return INSTANCE_PREPARE_MEMORY_FAILED;
        }
        if (!instance->path) {
            instance->path = paths ? copy_string(paths[index]) :
                monitor_query_path(raw->pid, raw->creation_time_utc_100ns, &instance->path_error);
            if (paths && paths[index] && !instance->path) return INSTANCE_PREPARE_MEMORY_FAILED;
            if (instance->path) {
                instance->path_error = 0;
                instance->program_id = monitor_store_program_id(engine->store, instance->path,
                    raw->process_name_utf8, raw->measured_at_utc_100ns);
                if (!instance->program_id) { store_result(engine, 0); return INSTANCE_PREPARE_DATABASE_FAILED; }
            } else if (paths) instance->path_error = ERROR_ACCESS_DENIED;
        }
    } else instance->path_error = ERROR_INVALID_DATA;
    if (!instance->window.start) {
        for (metric = 0; metric < 4; ++metric) {
            monitor_window_unavailable(&instance->report[metric], "identity/path unavailable");
        }
    }
    return INSTANCE_PREPARE_OK;
}

/* snapshot이 문자열 사본을 소유하며 배치·인스턴스의 저장 공간을 빌리지 않는다. */
static int clone_snapshot_row(MonitorRow *row, const ResourceData *raw,
    const MonitorInstance *instance)
{
    row->raw = *raw;
    row->name = copy_string(raw->process_name_utf8 ? raw->process_name_utf8 : "(unavailable)");
    row->path = copy_string(instance->path);
    row->raw.process_name_utf8 = row->name;
    if (!row->name || (instance->path && !row->path)) return 0;
    row->program_id = instance->program_id;
    row->path_error = instance->path_error;
    memcpy(row->metric, instance->report, sizeof(row->metric));
    return 1;
}

int monitor_ingest_with_commit_ref(MonitorEngine *e, const ResourceBatch *batch, const char *const *paths, MonitorSnapshot **out, MonitorCommitRef *ref)
{
    MonitorSnapshot *snapshot = NULL;
    MonitorInstance *next = NULL;
    int64_t run, cutoff;
    size_t i;
    LARGE_INTEGER begin, end, frequency;
    if (out) *out = NULL;
    if (ref) memset(ref, 0, sizeof(*ref));
    if (e) invalidate_commit(e);
    if (!e || !out) return 0;
    if (!valid_input(batch) || (paths && !e->synthetic)) {
        snprintf(e->error, sizeof(e->error), "Invalid batch or real DB fixture injection");
        monitor_reset(e);
        return 0;
    }
    e->error[0] = 0;
    QueryPerformanceCounter(&begin);
    QueryPerformanceFrequency(&frequency);
    /* 대기 상태를 분리해 두고 COMMIT 성공 뒤에만 engine·호출자에게 넘긴다. */
    snapshot = calloc(1, sizeof(*snapshot));
    next = calloc(batch->count ? batch->count : 1, sizeof(*next));
    if (!snapshot || !next) goto memory_failed;
    snapshot->rows = calloc(batch->count ? batch->count : 1, sizeof(*snapshot->rows));
    if (!snapshot->rows) goto memory_failed;
    snapshot->count = batch->count;
    snapshot->synthetic = e->synthetic;
    snapshot->measured_utc_100ns = batch->finished_at_utc_100ns;
    snapshot->foreground_observed_utc_100ns = batch->foreground_observed_at_utc_100ns;
    snapshot->foreground_pid = batch->foreground_pid;
    /* 이 회차의 창을 넣기 전에 과거 이력의 ID 상한을 고정한다. */
    if (!store_result(e, monitor_store_begin(e->store, &cutoff))) goto failed;
    baseline_history_begin(&e->history, e->store, cutoff);
    if (!store_result(e, monitor_store_insert_run(e->store, batch, &run))) goto failed;

    for (i = 0; i < batch->count; ++i) {
        const ResourceData *r = &batch->items[i];
        MonitorInstance *p = &next[i];
        MonitorRow *row = &snapshot->rows[i];
        InstancePrepareResult prepared = prepare_instance(e, p, r, paths, i);
        if (prepared == INSTANCE_PREPARE_MEMORY_FAILED) goto memory_failed;
        if (prepared == INSTANCE_PREPARE_DATABASE_FAILED) goto failed;
        if (!store_result(e, monitor_store_write_raw(e->store, run, r, p->program_id, p->path_error)) ||
            !advance_window(e, p, r, run, &snapshot->completed_windows)) goto failed;
        if (!clone_snapshot_row(row, r, p)) goto memory_failed;
        if (!store_result(e, monitor_store_read_feedback(e->store, row->program_id, batch->finished_at_utc_100ns, &row->feedback, &row->feedback_expires_utc_100ns))) goto failed;
    }
    /* 보관 정리가 cutoff 이하 행도 삭제할 수 있으므로 캐시 선택 결과를
     * trim이나 다음 트랜잭션까지 유지하지 않는다. */
    baseline_history_clear(&e->history);
    if (!store_result(e, monitor_store_touch_run_programs(e->store, run, batch->finished_at_utc_100ns)) ||
        !store_result(e, monitor_store_trim(e->store, batch->finished_at_utc_100ns)) ||
        !store_result(e, monitor_store_commit(e->store))) goto failed;

    /* 트랜잭션이 성공했으므로 새 인스턴스 배열의 소유권을 한 번만 넘긴다. */
    free_instances(e->instances, e->count);
    e->instances = next;
    e->count = batch->count;
    next = NULL;
    snapshot->sequence = (uint64_t)++e->sequence;
    if (ref && !e->refs_disabled) {
        ref->engine_cookie = e->cookie; ref->mutation_epoch = e->mutation_epoch;
        ref->sequence = snapshot->sequence; ref->session_id = e->session;
        ref->run_id = run; ref->history_cutoff = cutoff;
        e->committed_ref = *ref;
        e->committed_snapshot = snapshot; e->committed_rows = snapshot->rows;
        e->committed_count = snapshot->count; e->committed_windows = snapshot->completed_windows;
        e->committed_utc = snapshot->measured_utc_100ns;
        e->committed_foreground_utc = snapshot->foreground_observed_utc_100ns;
        e->committed_foreground_pid = snapshot->foreground_pid;
    }
    QueryPerformanceCounter(&end);
    snapshot->store_ms = (double)(end.QuadPart - begin.QuadPart) * 1000 / (double)frequency.QuadPart;
    snprintf(snapshot->status, sizeof(snapshot->status), "%srecorded %zu rows; %zu completed windows; local DB; raw 1h / summary 28d",
        e->synthetic ? "SYNTHETIC - " : "", snapshot->count, snapshot->completed_windows);
    *out = snapshot;
    return 1;
memory_failed:
    snprintf(e->error, sizeof(e->error), "Out of memory; window/streak state discarded");
failed:
    /* 미완료 DB 작업과 아직 공개하지 않은 메모리를 함께 버린다. */
    baseline_history_clear(&e->history);
    monitor_store_rollback(e->store);
    free_instances(next, batch->count);
    monitor_snapshot_free(snapshot);
    monitor_reset(e);
    return 0;
}

int monitor_ingest(MonitorEngine *e, const ResourceBatch *batch, const char *const *paths, MonitorSnapshot **out)
{ return monitor_ingest_with_commit_ref(e, batch, paths, out, NULL); }
int64_t monitor_source_session(const MonitorEngine *e) { return e ? e->session : 0; }
int monitor_source_create_pair(MonitorEngine *e, const char *pair)
{
    if (!e) return 0;
    invalidate_commit(e);
    if (!pair) { snprintf(e->error, sizeof(e->error), "Source pair unavailable"); return 0; }
    return store_result(e, monitor_store_source_create_pair(e->store, pair));
}
int monitor_source_read_committed(MonitorEngine *e, const MonitorCommitRef *ref,
    const MonitorSnapshot *snapshot, MonitorCommittedWindows *out)
{
    const MonitorCommitRef *expected;
    if (out) memset(out, 0, sizeof(*out));
    if (!e || !out) return 0;
    expected = &e->committed_ref;
    if (!ref || !snapshot || e->refs_disabled || !ref->engine_cookie ||
        ref->engine_cookie != e->cookie || ref->mutation_epoch != e->mutation_epoch ||
        ref->engine_cookie != expected->engine_cookie || ref->mutation_epoch != expected->mutation_epoch ||
        ref->sequence != expected->sequence || ref->session_id != expected->session_id ||
        ref->run_id != expected->run_id || ref->history_cutoff != expected->history_cutoff ||
        snapshot != e->committed_snapshot || snapshot->rows != e->committed_rows ||
        snapshot->sequence != ref->sequence || snapshot->count != e->committed_count ||
        snapshot->completed_windows != e->committed_windows || snapshot->synthetic != e->synthetic ||
        snapshot->measured_utc_100ns != e->committed_utc ||
        snapshot->foreground_observed_utc_100ns != e->committed_foreground_utc ||
        snapshot->foreground_pid != e->committed_foreground_pid) {
        snprintf(e->error, sizeof(e->error), "Committed source reference is stale or snapshot does not match");
        return 0;
    }
    return store_result(e, monitor_store_source_read(e->store, ref, snapshot, out));
}
