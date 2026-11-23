/* 회차별 저장 트랜잭션과 인스턴스 관측창·Baseline을 조율한다.
 * monitor_ingest_with_commit_ref에서 준비→저장→COMMIT→공개 순서를 읽는다.
 * 선택 상세의 최근 이력은 monitor_recent_read로만 조회하며, 수집·학습 경로와 분리한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "monitor_engine_private.h"
#include "monitor_completed.h"
#include "monitor_source_store.h"
#include "monitor_recent_store.h"

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
int monitor_retention_pending(const MonitorEngine *e) { return e ? e->retention_pending : 0; }
/* 성공한 회차/피드백마다 보관 정리를 시도한다. 실패하면 이미 커밋한 데이터는 유지하고
 * 경고만 남긴다. 다음 성공한 쓰기에서 재시도하며, 그 쓰기 자체가 실패하면 여기까지 오지
 * 않으므로 이전 경고와 실제 저장 오류가 함께 남고 monitor_error가 실제 실패를 설명한다. */
static void run_retention_after_commit(MonitorEngine *e, uint64_t now,
    MonitorInstance *instances, size_t count)
{
    int aggregate_changed = 0;
    size_t i;
    e->retention_pending = !monitor_store_trim_with_result(e->store, now, &aggregate_changed);
    if (!aggregate_changed) return;
    /* 최신 창이 남아도 과거 비교 범위가 삭제될 수 있고, 삭제된 ID도 재사용될 수 있다.
     * 값 캐시와 '창 없음'을 포함한 인스턴스별 조회 상태를 함께 비운다. ingest에서는
     * 곧 공개할 next를 받으므로 옛 배열을 비운 뒤 복사 상태가 되살아나지 않는다. */
    e->recent_cache_mask = 0;
    for (i = 0; i < count; ++i) {
        instances[i].recent_lookup_mask = 0;
        memset(instances[i].recent_anchor, 0, sizeof(instances[i].recent_anchor));
    }
}
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
/* program_id 단위 피드백을 저장하는 공개 진입점이다. GUI 작업 스레드가 이를 호출한다.
 * now는 UTC 100ns tick이며, 여기서 입력 검사와 억제 만료 시각을 정한다.
 * 잘못된 입력은 DB 행과 직전 오류를 보존하지만 최신 COMMIT 참조는 무효화한다.
 * 피드백 저장 실패는 0, COMMIT 뒤 보관 정리만 실패한 경우는 1과 retention_pending으로
 * 구분한다. SQL의 취소·추가·rollback은 monitor_store_commit_feedback에서 읽는다. */
int monitor_feedback(MonitorEngine *e, int64_t program, MonitorFeedback kind, uint64_t now)
{
    uint64_t duration = kind == MONITOR_FEEDBACK_IGNORE ? 3600 * MONITOR_TICKS_PER_SECOND : MONITOR_DAY_TICKS;
    uint64_t expires;
    if (!e) return 0;
    invalidate_commit(e);
    /* 취소도 기존 24시간 범위 검사를 거친다. 입력 오류는 직전 저장 오류를 보존한다. */
    if (program <= 0 || kind < MONITOR_FEEDBACK_CANCEL || kind > MONITOR_FEEDBACK_EXPECTED || now > INT64_MAX - duration) return 0;
    expires = kind == MONITOR_FEEDBACK_CANCEL ? now : now + duration;
    e->error[0] = 0;
    if (!store_result(e, monitor_store_commit_feedback(e->store, program, kind, now, expires))) return 0;
    run_retention_after_commit(e, now, e->instances, e->count);
    return 1;
}

void monitor_reset(MonitorEngine *e)
{
    if (!e) return;
    invalidate_commit(e);
    baseline_history_clear(&e->history);
    free_instances(e->instances, e->count);
    e->instances = NULL;
    e->count = 0;
    e->recent_first_run = 0;
    e->recent_cache_mask = 0;
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
/* 완료창에서 지표 하나의 품질을 먼저 검사하고 통과한 경우만 장기 기준과 비교한다.
 * 결측 지표의 연속 초과는 0으로 끊는다. SQL 실패는 회차 전체 rollback으로 전파한다. */
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
        if (p->report[m].state != MONITOR_UNAVAILABLE &&
            isfinite(p->report[m].current) && p->report[m].current >= 0) {
            p->recent_anchor[m] = scope.window_id;
            p->recent_lookup_mask |= 1u << m;
        }
        /* next 인스턴스에만 기록한다. 회차 COMMIT이 실패하면 이 상태도 버려진다.
         * 불충분한 새 완료창도 자신의 UNAVAILABLE로 교체하여 옛 상한을 빌리지 않는다. */
        p->completed.states = (p->completed.states & ~(UINT32_C(15) << (m * 4))) |
            ((uint32_t)(p->report[m].state + 1) << (m * 4));
        p->completed.metric[m].average = p->report[m].current;
        p->completed.metric[m].median = p->report[m].median;
        p->completed.metric[m].upper = p->report[m].threshold;
    }
    p->completed.start = p->window.start;
    p->completed.end = p->window.last_utc;
    p->completed.duration = p->window.duration;
    monitor_store_end_window(&scope);
    reset_window(p, p->window.last_utc, 0);
    return 1;
}
/* 현재 표본을 누적하고 완료된 창만 저장한다. 신원·시간 단절은 창 전체를 재시작하지만,
 * 한 지표 결측은 그 지표의 연속 초과·표시 상태만 무효화한다. 최신 유효 recent_anchor는 남긴다. */
static int advance_window(MonitorEngine *e, MonitorInstance *p, const ResourceData *r,
    int64_t run, size_t *completed)
{
    MonitorWindowStep step = monitor_window_observe(&p->window, r, p->program_id != 0);
    unsigned m;
    if (step.reset) {
        /* 긴 QPC 간격은 현재 창·연속 판정을 끊지만 이미 완료한 창의 출처는 바꾸지
         * 않는다. 신원 소실 또는 UTC 역행이면 그 근거도 버려 미래/다른 구간을 섞지 않는다. */
        if (!p->program_id || !(r->valid_fields & RESOURCE_VALID_IDENTITY) ||
            r->measured_at_utc_100ns <= p->window.last_utc)
            memset(&p->completed, 0, sizeof(p->completed));
        reset_window(p, r->measured_at_utc_100ns, 1);
        return 1;
    }
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

/* COMMIT이 끝난 회차만 공개한다. 추가 할당·SQL 없이 next의 소유권을 engine으로 넘기고,
 * snapshot은 호출자 소유로 남긴다. commit ref가 있으면 같은 snapshot의 주소·값을 빌려
 * 다음 변경 전까지의 검증 근거로 기록한다. 정리 지연도 이미 저장한 회차의 공개를 막지 않는다. */
static void publish_ingest(MonitorEngine *e, MonitorInstance *next, MonitorSnapshot *snapshot,
    int64_t run, int64_t cutoff, MonitorCommitRef *ref)
{
    free_instances(e->instances, e->count);
    e->instances = next;
    e->count = snapshot->count;
    if (!e->recent_first_run) e->recent_first_run = run;
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
}

/* 배치와 경로를 호출 중에만 빌려 다음 상태·snapshot을 준비한다. 회차 트랜잭션을 커밋하기
 * 전 실패하면 두 결과를 모두 버리고 관측 상태를 reset한다. 커밋 후에는 정리 시도와
 * publish_ingest만 남아 저장 성공을 실패로 바꾸지 않는다. out/ref는 진입 시 빈 값으로 둔다. */
int monitor_ingest_with_commit_ref(MonitorEngine *e, const ResourceBatch *batch, const char *const *paths, MonitorSnapshot **out, MonitorCommitRef *ref)
{
    MonitorSnapshot *snapshot = NULL;
    MonitorInstance *next = NULL;
    int64_t run, cutoff;
    int transaction_started = 0;
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
    transaction_started = 1;
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
        !store_result(e, monitor_store_commit(e->store))) goto failed;
    transaction_started = 0;
    run_retention_after_commit(e, batch->finished_at_utc_100ns, next, snapshot->count);

    publish_ingest(e, next, snapshot, run, cutoff, ref);
    next = NULL;
    QueryPerformanceCounter(&end);
    snapshot->store_ms = (double)(end.QuadPart - begin.QuadPart) * 1000 / (double)frequency.QuadPart;
    snprintf(snapshot->status, sizeof(snapshot->status), "%srecorded %zu rows; %zu completed windows; local DB; raw 1h / summary 28d%s",
        e->synthetic ? "SYNTHETIC - " : "", snapshot->count, snapshot->completed_windows,
        e->retention_pending ? "; retention cleanup delayed" : "");
    *out = snapshot;
    return 1;
memory_failed:
    snprintf(e->error, sizeof(e->error), "Out of memory; window/streak state discarded");
failed:
    /* 미완료 DB 작업과 아직 공개하지 않은 메모리를 함께 버린다. */
    baseline_history_clear(&e->history);
    if (transaction_started) monitor_store_rollback(e->store);
    free_instances(next, batch->count);
    monitor_snapshot_free(snapshot);
    monitor_reset(e);
    return 0;
}

int monitor_ingest(MonitorEngine *e, const ResourceBatch *batch, const char *const *paths, MonitorSnapshot **out)
{ return monitor_ingest_with_commit_ref(e, batch, paths, out, NULL); }

int monitor_completed_windows(MonitorEngine *e, const MonitorSnapshot *snapshot, MonitorCompletedWindow *out, size_t count)
{
    size_t i;
    if (!e || !snapshot || (count && !out) || count != e->count || count != snapshot->count ||
        snapshot->sequence != (uint64_t)e->sequence) return 0;
    for (i = 0; i < count; ++i) {
        const MonitorInstance *instance = &e->instances[i];
        const MonitorRow *row = &snapshot->rows[i];
        if (instance->pid != row->raw.pid || instance->creation != row->raw.creation_time_utc_100ns ||
            instance->program_id != row->program_id ||
            ((instance->path || row->path) && (!instance->path || !row->path || strcmp(instance->path, row->path)))) return 0;
        if (row->raw.valid_fields & RESOURCE_VALID_IDENTITY) out[i] = instance->completed;
        else memset(&out[i], 0, sizeof(out[i]));
    }
    return 1;
}
/* 행의 경로 문자열은 조회 동안만 빌린다. PID 재사용이나 같은 PID의 다른 프로그램 이력을
 * 붙이지 않도록 현재 engine 인스턴스의 생성시각·program ID·경로까지 함께 대조한다. */
static MonitorInstance *recent_instance_for_row(MonitorEngine *e, const MonitorRow *row)
{
    size_t i;
    for (i = 0; i < e->count; ++i) {
        MonitorInstance *p = &e->instances[i];
        if (p->pid == row->raw.pid && p->creation == row->raw.creation_time_utc_100ns &&
            p->program_id == row->program_id && p->path && !strcmp(p->path, row->path)) return p;
    }
    return NULL;
}

/* 한 지표에서 실제 마지막 저장 창과 현재 관측 시각에 보이는 창을 구분한다.
 * instance의 실제 anchor와 engine의 선택 캐시만 갱신하며, 호출자의 0 초기화된 임시 결과를
 * 채운다. SQL 실패는 0을 반환하고 최종 결과 공개 여부는 monitor_recent_read가 결정한다. */
static int read_recent_metric(MonitorEngine *e, MonitorInstance *instance, unsigned metric,
    uint64_t observed_utc, MonitorRecentMetric *out)
{
    unsigned bit = 1u << metric;
    MonitorRecentRequest request = {0};
    request.session_id = e->session; request.program_id = instance->program_id;
    request.first_run_id = e->recent_first_run;
    request.pid = instance->pid; request.metric = metric; request.creation = instance->creation;
    request.path = instance->path;
    if (!(instance->recent_lookup_mask & bit)) {
        /* 첫 조회는 UTC가 역행했어도 실제 마지막 저장 창을 찾는다. 그 ID를 유지해야 시계가
         * 다시 따라잡았을 때 과거의 '창 없음' 결과를 영구적으로 재사용하지 않는다. */
        request.as_of_utc_100ns = INT64_MAX;
        if (!monitor_store_recent_metric(e->store, &request, &e->recent_cache.metric[metric])) return 0;
        instance->recent_anchor[metric] = e->recent_cache.metric[metric].anchor_window_id;
        instance->recent_lookup_mask |= bit;
        e->recent_cache_anchor[metric] = instance->recent_anchor[metric];
        e->recent_cache_mask |= bit;
    }
    if (!instance->recent_anchor[metric]) return 1;
    if ((e->recent_cache_mask & bit) && e->recent_cache_anchor[metric] == instance->recent_anchor[metric] &&
        e->recent_cache.metric[metric].window_end_utc_100ns <= observed_utc) {
        *out = e->recent_cache.metric[metric];
        return 1;
    }
    request.anchor_window_id = instance->recent_anchor[metric];
    request.as_of_utc_100ns = observed_utc;
    if (!monitor_store_recent_metric(e->store, &request, out)) return 0;
    if (!out->anchor_window_id) {
        /* 실제 anchor가 현재 UTC보다 미래인 동안만 더 오래된 가시 창을 찾는다.
         * 대체 창으로 실제 ID를 바꾸거나 실제 anchor의 캐시를 덮어쓰지 않는다. */
        request.anchor_window_id = 0;
        if (!monitor_store_recent_metric(e->store, &request, out)) return 0;
    }
    if (out->anchor_window_id == instance->recent_anchor[metric]) {
        e->recent_cache_anchor[metric] = instance->recent_anchor[metric];
        e->recent_cache.metric[metric] = *out;
        e->recent_cache_mask |= bit;
    }
    return 1;
}

/* 최신 snapshot 행을 현재 인스턴스에 연결하고 모든 지표 조회 성공 뒤 값 사본을 공개한다.
 * 거부/SQL 실패 시 *out은 비어 있다. 일부 지표의 성공 캐시는 남아도 부분 결과는 반환하지 않는다.
 * DB 연결은 worker가 직렬 사용하며, 이 호출은 학습·알림·피드백 상태를 바꾸지 않는다. */
int monitor_recent_read(MonitorEngine *e, const MonitorRow *row, MonitorRecentComparison *out)
{
    MonitorInstance *instance;
    MonitorRecentComparison result = {0};
    unsigned metric;
    if (out) memset(out, 0, sizeof(*out));
    if (!e) return 0;
    e->error[0] = 0;
    if (!row || !out || !e->recent_first_run || !row->path || !row->path[0] ||
        row->path_error || row->program_id <= 0 || !(row->raw.valid_fields & RESOURCE_VALID_IDENTITY)) {
        snprintf(e->error, sizeof(e->error), "Recent comparison requires a current identified row and committed collection");
        return 0;
    }
    instance = recent_instance_for_row(e, row);
    if (!instance) {
        snprintf(e->error, sizeof(e->error), "Recent comparison instance is not in the current collection");
        return 0;
    }
    if (!store_result(e, monitor_store_require_idle(e->store))) return 0;
    if (e->recent_cached_pid != instance->pid || e->recent_cached_creation != instance->creation ||
        e->recent_cached_program != instance->program_id) {
        e->recent_cache_mask = 0;
        e->recent_cached_pid = instance->pid; e->recent_cached_creation = instance->creation;
        e->recent_cached_program = instance->program_id;
    }
    for (metric = 0; metric < MONITOR_METRICS; ++metric)
        if (!read_recent_metric(e, instance, metric, row->raw.measured_at_utc_100ns, &result.metric[metric]))
            return store_result(e, 0);
    *out = result;
    return 1;
}
int64_t monitor_source_session(const MonitorEngine *e) { return e ? e->session : 0; }
int monitor_source_create_pair(MonitorEngine *e, const char *pair)
{
    if (!e) return 0;
    invalidate_commit(e);
    if (!pair) { snprintf(e->error, sizeof(e->error), "Source pair unavailable"); return 0; }
    if (!store_result(e, monitor_store_require_idle(e->store))) return 0;
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
    if (!store_result(e, monitor_store_recover_pending(e->store))) return 0;
    return store_result(e, monitor_store_source_read(e->store, ref, snapshot, out));
}
