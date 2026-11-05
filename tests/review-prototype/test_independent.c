#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <psapi.h>
#include "experiment.h"
#include "sqlite3.h"

#define TICKS UINT64_C(10000000)
#define DAY (UINT64_C(86400) * TICKS)
#define MAX_ROWS 3
static unsigned checks, failures, serial;
static const char *area = "startup";
#define CHECK(v) do { ++checks; if (!(v)) { ++failures; printf("FAIL %s:%d %s\n", area, __LINE__, #v); } } while (0)
typedef struct Fixture {
    Experiment *e;
    ResourceData rows[MAX_ROWS];
    ResourceBatch b;
    const char *paths[MAX_ROWS];
    MonitorSnapshot *s;
    uint64_t now;
    char source[MAX_PATH], evidence[MAX_PATH];
} Fixture;
static void fatal(const char *message) { printf("FATAL %s\n", message); exit(2); }
static void path(char *out, size_t capacity, const char *label)
{
    snprintf(out, capacity, "build-review-prototype/fixture-%lu-%u-%s.sqlite", (unsigned long)GetCurrentProcessId(), ++serial, label);
    CHECK(GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES);
}
static sqlite3 *db_open(const char *file)
{
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(file, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) fatal(db ? sqlite3_errmsg(db) : "db open");
    return db;
}
static int execute(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) printf("SQL failure: %s\n", error ? error : sqlite3_errmsg(db));
    sqlite3_free(error); return rc == SQLITE_OK;
}
static int64_t number(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL; int64_t result = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) result = sqlite3_column_int64(s, 0);
    else CHECK(0);
    sqlite3_finalize(s); return result;
}
static void init(Fixture *f, unsigned count)
{
    unsigned i; char error[512];
    memset(f, 0, sizeof(*f));
    path(f->source, sizeof(f->source), "source"); path(f->evidence, sizeof(f->evidence), "evidence");
    if (!experiment_create(f->source, f->evidence, 1, &f->e, error, sizeof(error))) fatal(error);
    f->now = monitor_utc_now() / DAY * DAY + UINT64_C(43200) * TICKS;
    f->b.items = f->rows; f->b.count = count; f->b.logical_processor_count = 8;
    for (i = 0; i < count; ++i) {
        ResourceData *r = &f->rows[i];
        r->pid = 3100 + i; r->creation_time_utc_100ns = f->now - 3600 * TICKS;
        r->process_name_utf8 = "same.exe"; f->paths[i] = "SYNTHETIC:\\independent\\same.exe";
        r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->cpu_percent = 10; r->working_set_bytes = UINT64_C(67108864);
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->foreground = RESOURCE_FOREGROUND_UNKNOWN;
    }
}
static int tick(Fixture *f, double seconds)
{
    size_t i; int ok;
    f->now += (uint64_t)(seconds * (double)TICKS);
    for (i = 0; i < f->b.count; ++i) f->rows[i].measured_at_utc_100ns = f->now;
    f->b.started_at_utc_100ns = f->b.finished_at_utc_100ns = f->now;
    monitor_snapshot_free(f->s); f->s = NULL;
    ok = experiment_ingest(f->e, &f->b, f->paths, &f->s);
    if (!ok) printf("ingest returned: %s\n", experiment_error(f->e));
    return ok;
}
static void window(Fixture *f)
{
    unsigned i;
    for (i = 0; i < 20; ++i) CHECK(tick(f, 3));
    CHECK(f->s && f->s->completed_windows == f->b.count);
}
static void next_completed_window(Fixture *f)
{
    unsigned i;
    for (i = 0; i < 25; ++i) {
        CHECK(tick(f, 3));
        if (f->s && f->s->completed_windows == f->b.count) return;
    }
    CHECK(0);
}
static void done(Fixture *f)
{
    monitor_snapshot_free(f->s); f->s = NULL; experiment_close(f->e); f->e = NULL;
    printf("ARTIFACT source=%s evidence=%s\n", f->source, f->evidence);
}
static ExperimentStats stats(Fixture *f)
{
    ExperimentStats result;
    memset(&result, 0, sizeof(result)); CHECK(experiment_stats(f->e, &result)); return result;
}
static ExperimentDecision *decisions(Fixture *f, ExperimentMethod method, size_t *count)
{
    ExperimentDecision *d = NULL;
    CHECK(experiment_decisions(f->e, method, 10000, 0, &d, count)); return d;
}
static ExperimentEpisode *episodes(Fixture *f, int method, size_t *count)
{
    ExperimentEpisode *e = NULL;
    CHECK(experiment_episodes(f->e, method, 10000, 0, &e, count)); return e;
}
static void seed(Fixture *f, double cpu)
{
    sqlite3 *db = db_open(f->source);
    sqlite3_stmt *w = NULL, *m = NULL;
    int64_t run = number(db, "SELECT max(id) FROM collection_run"), program = number(db, "SELECT max(id) FROM program");
    unsigned i, metric;
    CHECK(execute(db, "BEGIN IMMEDIATE"));
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,1,1,?,?,60,1)", -1, &w, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO aggregate_metric VALUES(?,?,?,60,20,1,1,'review-seed',0,0,0,NULL,NULL,NULL,0)", -1, &m, NULL) == SQLITE_OK);
    for (i = 0; i < 60; ++i) {
        uint64_t end = (f->now / DAY - (1 + i % 3)) * DAY + (uint64_t)(i / 3 + 1) * 60 * TICKS;
        int64_t id;
        sqlite3_bind_int64(w, 1, run); sqlite3_bind_int64(w, 2, program);
        sqlite3_bind_int64(w, 3, (sqlite3_int64)(end - 60 * TICKS)); sqlite3_bind_int64(w, 4, (sqlite3_int64)end);
        CHECK(sqlite3_step(w) == SQLITE_DONE); sqlite3_reset(w); id = sqlite3_last_insert_rowid(db);
        for (metric = 0; metric < 4; ++metric) {
            sqlite3_bind_int64(m, 1, id); sqlite3_bind_int(m, 2, (int)metric);
            sqlite3_bind_double(m, 3, metric == 0 ? cpu : metric == 1 ? 67108864 : 0);
            CHECK(sqlite3_step(m) == SQLITE_DONE); sqlite3_reset(m);
        }
    }
    sqlite3_finalize(w); sqlite3_finalize(m); CHECK(execute(db, "COMMIT")); sqlite3_close(db);
}
static void test_unready_fixed_and_zero(void)
{
    Fixture f; ExperimentDecision *d; ExperimentEpisode *e; ExperimentStats s; size_t n; unsigned i;
    area = "unready/fixed/zero"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40;
    window(&f); window(&f); s = stats(&f); CHECK(s.episodes == 0);
    window(&f); s = stats(&f); CHECK(s.windows == 3 && s.decisions == 24 && s.active == 1);
    d = decisions(&f, EXPERIMENT_PERSONALIZED, &n); CHECK(n == 12);
    for (i = 0; i < n; ++i) CHECK(d[i].report.state == MONITOR_LEARNING);
    experiment_decisions_free(d, n);
    d = decisions(&f, EXPERIMENT_FIXED, &n); CHECK(n == 12);
    for (i = 0; i < n; ++i) if (d[i].metric == MONITOR_CPU && d[i].end_utc == f.now) {
        CHECK(d[i].report.state == MONITOR_CANDIDATE && d[i].report.streak == 3);
        CHECK(d[i].report.threshold == 25 && d[i].report.current == 40);
    }
    experiment_decisions_free(d, n);
    e = episodes(&f, EXPERIMENT_FIXED, &n); CHECK(n == 1);
    if (n) { CHECK(e[0].state == EXPERIMENT_ACTIVE); CHECK(e[0].pid == f.rows[0].pid); }
    experiment_episodes_free(e, n);
    window(&f); CHECK(stats(&f).episodes == 1);
    f.rows[0].cpu_percent = 0; f.rows[0].working_set_bytes = 0; window(&f);
    e = episodes(&f, EXPERIMENT_FIXED, &n); CHECK(n == 1 && e[0].state == EXPERIMENT_RECOVERED);
    experiment_episodes_free(e, n);
    d = decisions(&f, EXPERIMENT_FIXED, &n);
    for (i = 0; i < n; ++i) if (d[i].end_utc == f.now) CHECK(d[i].report.state == MONITOR_NORMAL && d[i].report.current == 0);
    experiment_decisions_free(d, n); done(&f);
}
static void test_personalized_and_fixed_independence(void)
{
    Fixture f; ExperimentStats s; ExperimentDecision *d; ExperimentEpisode *e; size_t n; unsigned i;
    area = "personalized/fixed independence"; init(&f, 1); CHECK(tick(&f, 0)); seed(&f, 30);
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f);
    s = stats(&f); CHECK(s.active == 1 && s.episodes == 1);
    d = decisions(&f, EXPERIMENT_PERSONALIZED, &n);
    for (i = 0; i < n; ++i) if (d[i].metric == MONITOR_CPU) {
        CHECK(d[i].report.state == MONITOR_NORMAL); CHECK(d[i].report.threshold == 45);
        CHECK(d[i].report.count >= 60 && d[i].report.days >= 3);
    }
    experiment_decisions_free(d, n);
    e = episodes(&f, -1, &n); CHECK(n == 1 && e[0].method == EXPERIMENT_FIXED);
    experiment_episodes_free(e, n); done(&f);
}
static void test_missing_resets_immediately(void)
{
    Fixture f; ExperimentEpisode *e; ExperimentStats s; size_t n;
    area = "missing immediate reset"; init(&f, 1); CHECK(tick(&f, 0)); seed(&f, 10);
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f); CHECK(stats(&f).active == 2);
    f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_CPU; CHECK(tick(&f, 3));
    s = stats(&f); CHECK(s.active == 0 && s.unknown == 2 && s.recovered == 0);
    e = episodes(&f, -1, &n); CHECK(n == 2);
    experiment_episodes_free(e, n);
    f.rows[0].valid_fields |= RESOURCE_VALID_CPU;
    while (f.s->completed_windows == 0) CHECK(tick(&f, 3));
    CHECK(stats(&f).active == 0); window(&f); CHECK(stats(&f).active == 0); window(&f);
    CHECK(stats(&f).active == 2 && stats(&f).episodes == 4); done(&f);
}
static void test_same_name_pid_reuse_absence(void)
{
    Fixture f; ExperimentEpisode *e; ExperimentStats s; size_t n; unsigned i;
    area = "same name / PID reuse / absence"; init(&f, 3);
    f.paths[1] = "SYNTHETIC:\\other\\same.exe";
    CHECK(tick(&f, 0));
    for (i = 0; i < 3; ++i) f.rows[i].cpu_percent = 40;
    window(&f); window(&f); window(&f); CHECK(stats(&f).active == 3);
    f.rows[0].creation_time_utc_100ns += 1;
    CHECK(tick(&f, 3)); s = stats(&f); CHECK(s.active == 2 && s.unknown == 1);
    f.b.count = 1; CHECK(tick(&f, 3)); s = stats(&f); CHECK(s.active == 0 && s.unknown == 3);
    e = episodes(&f, -1, &n); CHECK(n == 3);
    experiment_episodes_free(e, n); done(&f);
}
static void test_past_future_and_same_cycle(void)
{
    Fixture f; sqlite3 *db; ExperimentDecision *d; size_t n; unsigned i; char sql[512];
    area = "past/future/same-cycle cutoff"; init(&f, 2); CHECK(tick(&f, 0)); seed(&f, 10);
    db = db_open(f.source);
    CHECK(execute(db, "DELETE FROM aggregate_metric WHERE window_id=(SELECT max(id) FROM aggregate_window)"));
    snprintf(sql, sizeof(sql), "UPDATE aggregate_window SET start_utc=%llu,end_utc=%llu WHERE id=(SELECT max(id) FROM aggregate_window)",
        (unsigned long long)(f.now + DAY), (unsigned long long)(f.now + DAY + 60 * TICKS));
    CHECK(execute(db, sql));
    CHECK(execute(db, "INSERT INTO aggregate_metric VALUES((SELECT max(id) FROM aggregate_window),0,1,60,20,1,1,'future',0,0,0,NULL,NULL,NULL,0)"));
    sqlite3_close(db); window(&f);
    CHECK(f.s->rows[0].metric[MONITOR_CPU].count == 59 && f.s->rows[1].metric[MONITOR_CPU].count == 59);
    d = decisions(&f, EXPERIMENT_PERSONALIZED, &n); CHECK(n == 8);
    for (i = 0; i < n; ++i) if (d[i].metric == MONITOR_CPU) {
        CHECK(d[i].report.count == 59 && d[i].report.state == MONITOR_LEARNING);
        CHECK(d[i].source_window_id > d[i].source_cutoff);
    }
    experiment_decisions_free(d, n); done(&f);
}
static void test_restart_history_replay_export_evaluation(void)
{
    Fixture f; Experiment *replay = NULL; ExperimentStats a, b; ExperimentEpisode *e;
    ExperimentDecision *d; ExperimentEvaluation v; size_t n; char error[512], target[MAX_PATH], csv[MAX_PATH], retained[MAX_PATH];
    area = "restart/history/replay/export/evaluation"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    experiment_close(f.e); f.e = NULL;
    CHECK(experiment_resume(f.evidence, &f.e, error, sizeof(error)));
    if (!f.e) fatal(error);
    a = stats(&f); CHECK(a.active == 0 && a.unknown == 1);
    CHECK(tick(&f, 3)); window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    d = decisions(&f, EXPERIMENT_FIXED, &n); CHECK(n == 24);
    memset(&v, 0, sizeof(v));
    v.decision_id = n ? d[0].id : 0; v.trial_id = "independent-noncandidate-trial"; v.order_id = "fixed-first";
    v.judgment = EXPERIMENT_UNSURE;
    v.understood = v.target_correct = v.time_correct = v.metric_correct = v.next_check = -1;
    v.elapsed_seconds = -1; v.note = "manual fixture; no human effectiveness claim, comma, quote\" newline\n";
    CHECK(experiment_evaluate(f.e, &v)); CHECK(stats(&f).evaluations == 1);
    v.decision_id = INT64_C(99999999); CHECK(!experiment_evaluate(f.e, &v)); CHECK(stats(&f).evaluations == 1);
    v.decision_id = n ? d[0].id : 0; v.understood = 2; CHECK(!experiment_evaluate(f.e, &v));
    v.understood = -1; v.elapsed_seconds = NAN; CHECK(!experiment_evaluate(f.e, &v));
    experiment_decisions_free(d, n);
    path(csv, sizeof(csv), "export.csv"); CHECK(experiment_export(f.e, csv)); CHECK(!experiment_export(f.e, csv));
    CHECK(experiment_finish(f.e, f.now)); a = stats(&f); CHECK(a.active == 0 && a.unknown == 2);
    experiment_close(f.e); f.e = NULL;
    snprintf(retained, sizeof(retained), "%s.retained", f.source);
    CHECK(MoveFileA(f.source, retained));
    CHECK(!experiment_resume(f.evidence, &f.e, error, sizeof(error)) && !f.e);
    CHECK(experiment_open_history(f.evidence, &f.e, error, sizeof(error)));
    if (!f.e) fatal(error);
    CHECK(!experiment_evaluate(f.e, &v));
    a = stats(&f); CHECK(!a.source_available);
    path(target, sizeof(target), "replay");
    CHECK(experiment_replay(f.evidence, target, error, sizeof(error)));
    CHECK(!experiment_replay(f.evidence, target, error, sizeof(error)));
    CHECK(experiment_open_history(target, &replay, error, sizeof(error)));
    if (!replay) fatal(error);
    CHECK(experiment_stats(replay, &b));
    CHECK(a.windows == b.windows && a.decisions == b.decisions && a.episodes == b.episodes);
    CHECK(a.active == b.active && a.recovered == b.recovered && a.unknown == b.unknown && a.gaps == b.gaps);
    CHECK(memcmp(a.states, b.states, sizeof(a.states)) == 0 && b.replay_only);
    e = episodes(&f, -1, &n); CHECK(n == 2); experiment_episodes_free(e, n);
    experiment_close(replay); done(&f);
    printf("ARTIFACT replay=%s export=%s retained-source=%s\n", target, csv, retained);
}
static void test_unrecognized_guard(void)
{
    Experiment *e = NULL; sqlite3 *db = NULL; char file[MAX_PATH], fresh[MAX_PATH], error[512];
    area = "unrecognized DB preservation";
    path(file, sizeof(file), "ordinary"); path(fresh, sizeof(fresh), "must-not-create");
    CHECK(sqlite3_open(file, &db) == SQLITE_OK);
    CHECK(execute(db, "CREATE TABLE preserved(s TEXT);INSERT INTO preserved VALUES('do not change');PRAGMA application_id=555;PRAGMA user_version=8"));
    sqlite3_close(db); db = NULL;
    CHECK(!experiment_create(file, fresh, 1, &e, error, sizeof(error)) && !e);
    CHECK(GetFileAttributesA(fresh) == INVALID_FILE_ATTRIBUTES);
    CHECK(!experiment_resume(file, &e, error, sizeof(error)) && !e);
    CHECK(!experiment_open_history(file, &e, error, sizeof(error)) && !e);
    CHECK(!experiment_open_review(file, &e, error, sizeof(error)) && !e);
    db = db_open(file);
    CHECK(number(db, "PRAGMA application_id") == 555 && number(db, "PRAGMA user_version") == 8);
    CHECK(number(db, "SELECT count(*) FROM preserved WHERE s='do not change'") == 1);
    CHECK(number(db, "SELECT count(*) FROM sqlite_master WHERE type='table'") == 1);
    sqlite3_close(db); printf("ARTIFACT guarded=%s\n", file);
}
static void test_rollback_and_sidecar_failure(void)
{
    Fixture f; sqlite3 *db; int64_t runs, evidence; char error[512];
    area = "core rollback / sidecar partial commit"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    db = db_open(f.source); runs = number(db, "SELECT count(*) FROM collection_run");
    CHECK(execute(db, "CREATE TRIGGER review_block_raw BEFORE INSERT ON raw BEGIN SELECT RAISE(ABORT,'independent injected core failure'); END"));
    sqlite3_close(db); CHECK(!tick(&f, 3) && !f.s);
    db = db_open(f.source); CHECK(number(db, "SELECT count(*) FROM collection_run") == runs);
    CHECK(execute(db, "DROP TRIGGER review_block_raw")); sqlite3_close(db);
    CHECK(experiment_reset(f.e, f.now, "recover injected core failure"));
    CHECK(stats(&f).active == 0 && stats(&f).unknown == 1);
    CHECK(tick(&f, 3)); window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    db = db_open(f.evidence); evidence = number(db, "SELECT count(*) FROM operation");
    CHECK(execute(db, "CREATE TRIGGER review_block_evidence BEFORE INSERT ON operation BEGIN SELECT RAISE(ABORT,'independent injected evidence failure'); END"));
    sqlite3_close(db);
    /* Missing metric forces a gap insertion after the core transaction commits. */
    f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_CPU;
    db = db_open(f.source); runs = number(db, "SELECT count(*) FROM collection_run"); sqlite3_close(db);
    CHECK(!tick(&f, 3) && !f.s); CHECK(strstr(experiment_error(f.e), "Core committed") != NULL);
    db = db_open(f.source); CHECK(number(db, "SELECT count(*) FROM collection_run") == runs + 1); sqlite3_close(db);
    db = db_open(f.evidence); CHECK(number(db, "SELECT count(*) FROM operation") == evidence);
    CHECK(execute(db, "DROP TRIGGER review_block_evidence")); sqlite3_close(db);
    CHECK(!tick(&f, 3) && !f.s); /* fault requires explicit recovery */
    experiment_close(f.e); f.e = NULL;
    CHECK(experiment_resume(f.evidence, &f.e, error, sizeof(error)));
    if (!f.e) fatal(error);
    CHECK(stats(&f).active == 0 && stats(&f).unknown == 2 && stats(&f).recovered == 0);
    done(&f);
}
static void test_retained_replay_boundary(void)
{
    Fixture f; sqlite3 *db; ExperimentEpisode *before, *after; Experiment *replay = NULL;
    size_t n, k; char sql[1024], target[MAX_PATH], error[512];
    uint64_t shift = 29 * DAY;
    area = "retained replay prefix boundary"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f); window(&f);
    /* Dedicated projection fixture: expire the first three windows while a
       current continuation retains the episode. This is not a 29-day live run. */
    db = db_open(f.evidence);
    snprintf(sql, sizeof(sql), "UPDATE operation SET utc=utc-%llu WHERE kind=2 AND id IN(SELECT id FROM window_evidence ORDER BY id LIMIT 3);"
        "UPDATE window_evidence SET start=start-%llu,end=end-%llu WHERE id IN(SELECT id FROM window_evidence ORDER BY id LIMIT 3);"
        "UPDATE episode SET first=first-%llu,activated=activated-%llu;",
        (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift);
    CHECK(execute(db, sql)); sqlite3_close(db);
    CHECK(tick(&f, 3)); before = episodes(&f, -1, &n); CHECK(n == 1 && before[0].state == EXPERIMENT_ACTIVE);
    CHECK(stats(&f).windows == 1); path(target, sizeof(target), "retained-replay");
    CHECK(experiment_replay(f.evidence, target, error, sizeof(error)));
    CHECK(experiment_open_history(target, &replay, error, sizeof(error)));
    if (!replay) fatal(error);
    CHECK(experiment_episodes(replay, -1, 100, 0, &after, &k)); CHECK(k == n);
    if (n && k) {
        CHECK(before[0].first_utc == after[0].first_utc && before[0].activated_utc == after[0].activated_utc);
        CHECK(before[0].qualifying_windows == after[0].qualifying_windows && before[0].state == after[0].state);
        CHECK(before[0].activation_decision_id == after[0].activation_decision_id);
    }
    experiment_episodes_free(before, n); experiment_episodes_free(after, k); experiment_close(replay); done(&f);
    printf("ARTIFACT retained-replay=%s\n", target);
}
static void test_gap_duplicate_and_late_order(void)
{
    Fixture f; ExperimentDecision *d = NULL; size_t n; ExperimentStats z;
    area = "gap / duplicate / reversed UTC"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    CHECK(tick(&f, 0)); z = stats(&f); CHECK(z.active == 0 && z.unknown == 1 && z.recovered == 0 && z.windows == 3);
    window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    f.now -= 6 * TICKS; CHECK(tick(&f, 3)); CHECK(stats(&f).active == 0 && stats(&f).unknown == 2);
    f.rows[0].observation_interval_seconds = 20; CHECK(tick(&f, 20));
    f.rows[0].observation_interval_seconds = 3; window(&f); window(&f); window(&f); CHECK(stats(&f).active == 1);
    CHECK(experiment_reset(f.e, f.now, "explicit collector failure")); z = stats(&f);
    CHECK(z.active == 0 && z.unknown == 3 && z.recovered == 0);
    CHECK(experiment_decisions(f.e, EXPERIMENT_FIXED, 2, 0, &d, &n) && n == 2);
    if (n == 2) CHECK(d[0].id > d[1].id);
    experiment_decisions_free(d, n);
    CHECK(!experiment_decisions(f.e, EXPERIMENT_FIXED, 10001, 0, &d, &n));
    done(&f);
}
static void test_retained_recovery_only(void)
{
    Fixture f; sqlite3 *db; ExperimentEpisode *before = NULL, *after = NULL; Experiment *replay = NULL;
    size_t n = 0, k = 0; char sql[1400], target[MAX_PATH], error[512]; uint64_t shift = 29 * DAY;
    area = "retained recovery-only replay"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f);
    f.rows[0].cpu_percent = 0; window(&f); CHECK(stats(&f).recovered == 1);
    db = db_open(f.evidence);
    snprintf(sql, sizeof(sql), "UPDATE operation SET utc=utc-%llu WHERE kind=2 AND id IN(SELECT id FROM window_evidence ORDER BY id LIMIT 3);"
        "UPDATE window_evidence SET start=start-%llu,end=end-%llu WHERE id IN(SELECT id FROM window_evidence ORDER BY id LIMIT 3);"
        "UPDATE episode SET first=first-%llu,activated=activated-%llu,last=last-%llu;",
        (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift,
        (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift);
    CHECK(execute(db, sql)); sqlite3_close(db); CHECK(tick(&f, 3));
    before = episodes(&f, -1, &n); CHECK(n == 1 && before[0].state == EXPERIMENT_RECOVERED);
    CHECK(stats(&f).windows == 1); path(target, sizeof(target), "recovery-only-replay");
    if (experiment_replay(f.evidence, target, error, sizeof(error))) {
        CHECK(experiment_open_history(target, &replay, error, sizeof(error)));
        if (replay) {
            CHECK(experiment_episodes(replay, -1, 100, 0, &after, &k)); CHECK(k == n);
            if (n && k) CHECK(before[0].id == after[0].id && before[0].first_utc == after[0].first_utc &&
                before[0].state == after[0].state && before[0].ended_utc == after[0].ended_utc && before[0].last_decision_id == after[0].last_decision_id);
        }
    } else { CHECK(0); printf("REPLAY error: %s\n", error); }
    experiment_episodes_free(before, n); experiment_episodes_free(after, k); experiment_close(replay); done(&f);
}
static void test_full_replay_recomputes_quality(void)
{
    Fixture f; sqlite3 *db; char target[MAX_PATH], error[512]; int64_t changed;
    area = "full replay independent quality validation"; init(&f, 1); CHECK(tick(&f, 0)); window(&f);
    db = db_open(f.evidence);
    changed = number(db, "SELECT id FROM decision WHERE method=1 AND metric=0");
    CHECK(execute(db, "UPDATE decision SET state=0,streak=0 WHERE method=1 AND metric=0"));
    sqlite3_close(db); path(target, sizeof(target), "tampered-fixed-replay");
    CHECK(!experiment_replay(f.evidence, target, error, sizeof(error)));
    CHECK(strstr(error, "mismatch") != NULL);
    db = db_open(f.evidence);
    CHECK(number(db, "SELECT state FROM decision WHERE method=1 AND metric=0") == MONITOR_UNAVAILABLE);
    CHECK(number(db, "SELECT id FROM decision WHERE method=1 AND metric=0") == changed);
    sqlite3_close(db); done(&f);
}
static void test_exposed_ids_never_reused(void)
{
    Fixture f; sqlite3 *db; ExperimentDecision *d = NULL, *stale = NULL; ExperimentEpisode *e = NULL;
    size_t n; int64_t old_decision, old_episode, new_decision, new_episode; char sql[1200]; uint64_t shift = 29 * DAY;
    area = "durable exposed IDs after retention"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; window(&f); window(&f); window(&f);
    d = decisions(&f, EXPERIMENT_FIXED, &n); old_decision = n ? d[0].id : 0; experiment_decisions_free(d, n);
    e = episodes(&f, EXPERIMENT_FIXED, &n); old_episode = n ? e[0].id : 0; experiment_episodes_free(e, n);
    CHECK(experiment_finish(f.e, f.now));
    db = db_open(f.evidence);
    snprintf(sql, sizeof(sql), "UPDATE operation SET utc=utc-%llu;UPDATE window_evidence SET start=start-%llu,end=end-%llu;"
        "UPDATE episode SET first=first-%llu,activated=activated-%llu,last=last-%llu,ended=ended-%llu;",
        (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift,
        (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift, (unsigned long long)shift);
    CHECK(execute(db, sql)); sqlite3_close(db);
    ++f.rows[0].pid; ++f.rows[0].creation_time_utc_100ns; CHECK(tick(&f, 3));
    CHECK(stats(&f).windows == 0 && stats(&f).episodes == 0);
    window(&f); window(&f); window(&f);
    d = decisions(&f, EXPERIMENT_FIXED, &n); new_decision = n ? d[0].id : 0; experiment_decisions_free(d, n);
    e = episodes(&f, EXPERIMENT_FIXED, &n); new_episode = n ? e[0].id : 0; experiment_episodes_free(e, n);
    CHECK(new_decision > old_decision && new_episode > old_episode);
    CHECK(!experiment_get_decision(f.e, old_decision, &stale) && !stale);
    done(&f);
}
static void test_companion_guard_and_formula_text(void)
{
    char source[MAX_PATH], side[MAX_PATH], companion[MAX_PATH], error[512], csv[MAX_PATH];
    HANDLE h; DWORD written; Experiment *e = NULL; Fixture f; FILE *out; char line[8192]; int saw_formula = 0;
    area = "companion preservation / CSV text boundary";
    path(source, sizeof(source), "orphan-companion-source"); path(side, sizeof(side), "orphan-companion-evidence");
    snprintf(companion, sizeof(companion), "%s-wal", source);
    h = CreateFileA(companion, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(WriteFile(h, "preserved orphan companion", 26, &written, NULL) && written == 26); CloseHandle(h);
    }
    CHECK(!experiment_create(source, side, 1, &e, error, sizeof(error)) && !e);
    CHECK(GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(side) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA(companion) != INVALID_FILE_ATTRIBUTES);
    init(&f, 1); f.rows[0].process_name_utf8 = "=1+1"; CHECK(tick(&f, 0)); window(&f);
    path(csv, sizeof(csv), "formula-text-export.csv"); CHECK(experiment_export(f.e, csv));
    out = NULL; CHECK(fopen_s(&out, csv, "rb") == 0 && out);
    if (out) {
        while (fgets(line, sizeof(line), out)) if (strstr(line, "=1+1")) {
            saw_formula = 1; CHECK(strncmp(line, "\"window\",", 9) == 0 && strstr(line, ",{\"") == NULL);
            CHECK(strstr(line, ",\"{\"\"") != NULL); /* controlled CSV type/id, JSON object payload */
        }
        fclose(out);
    }
    CHECK(saw_formula); done(&f);
}
static void test_latest_source_quality(void)
{
    Fixture f; ExperimentStats s; sqlite3 *db; int64_t runs, raw;
    area = "latest source raw quality / expiry"; init(&f, 3);
    f.rows[0].cpu_percent = 0; f.rows[0].working_set_bytes = 0;
    f.rows[1].valid_fields &= ~(unsigned)(RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE);
    f.paths[2] = NULL; f.rows[2].cpu_percent = NAN;
    CHECK(tick(&f, 0));
    db = db_open(f.source); runs = number(db, "SELECT count(*) FROM collection_run"); raw = number(db, "SELECT count(*) FROM raw"); sqlite3_close(db);
    s = stats(&f);
    CHECK(s.source_available && s.latest_source_available && s.latest_source_status == 0);
    CHECK(s.latest_source_rows == 3 && s.latest_source_raw_rows == 3 && s.latest_source_utc == f.now);
    CHECK(s.latest_source_identity_unavailable == 1 && s.latest_source_path_unavailable == 2);
    CHECK(s.latest_source_metric_missing[MONITOR_CPU] == 2);
    CHECK(s.latest_source_metric_missing[MONITOR_RAM] == 1 && s.latest_source_metric_missing[MONITOR_READ] == 1 && s.latest_source_metric_missing[MONITOR_WRITE] == 1);
    db = db_open(f.source);
    CHECK(number(db, "SELECT count(*) FROM collection_run") == runs && number(db, "SELECT count(*) FROM raw") == raw);
    CHECK(execute(db, "DELETE FROM raw WHERE id=(SELECT max(id) FROM raw)")); sqlite3_close(db);
    s = stats(&f); CHECK(s.source_available && !s.latest_source_available && s.latest_source_rows == 3 && s.latest_source_raw_rows == 2);
    done(&f);
}
static void test_repeated_invalid_identity_has_no_fake_gaps(void)
{
    Fixture f; ExperimentStats s; uint64_t gaps; unsigned i;
    area = "invalid identity repetition / valid break"; init(&f, 1);
    f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_IDENTITY; /* stale nonzero creation intentionally retained */
    for (i = 0; i < 10; ++i) CHECK(tick(&f, 3));
    CHECK(stats(&f).gaps == 0 && stats(&f).windows == 0);
    f.rows[0].valid_fields |= RESOURCE_VALID_IDENTITY; CHECK(tick(&f, 3));
    f.rows[0].cpu_percent = 40; next_completed_window(&f); next_completed_window(&f); next_completed_window(&f); CHECK(stats(&f).active == 1);
    gaps = stats(&f).gaps; f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_IDENTITY;
    CHECK(tick(&f, 3)); s = stats(&f); CHECK(s.gaps == gaps + 1 && s.active == 0 && s.unknown == 1);
    for (i = 0; i < 10; ++i) CHECK(tick(&f, 3));
    CHECK(stats(&f).gaps == gaps + 1 && stats(&f).unknown == 1);
    f.rows[0].valid_fields |= RESOURCE_VALID_IDENTITY; CHECK(tick(&f, 3));
    next_completed_window(&f); next_completed_window(&f); next_completed_window(&f); s = stats(&f);
    CHECK(s.active == 1 && s.unknown == 1 && s.episodes == 2 && s.gaps == gaps + 1);
    done(&f);
}
static void test_long_utf8_explanation_boundary(void)
{
    ExperimentDecision d; char name[6001], path_text[6001], output[16384], tiny[16]; unsigned i;
    uint64_t now = monitor_utc_now();
    area = "bounded UTF8 evidence explanation"; memset(&d, 0, sizeof(d));
    for (i = 0; i < 6000; i += 3) { memcpy(name + i, "\xEA\xB0\x80", 3); memcpy(path_text + i, "\xEB\x82\x98", 3); }
    name[6000] = path_text[6000] = 0;
    d.name = name; d.path = path_text; d.method = EXPERIMENT_FIXED; d.metric = MONITOR_CPU;
    d.start_utc = now - 180 * TICKS; d.end_utc = now - 120 * TICKS;
    d.report.state = MONITOR_NORMAL; d.report.current = 0; d.report.threshold = 25;
    d.report.median = d.report.mad = NAN;
    d.report.window_start_utc_100ns = d.start_utc; d.report.window_end_utc_100ns = d.end_utc;
    d.report.window_duration_seconds = 60; d.report.covered_seconds = 60; d.report.valid_samples = 20;
    CHECK(experiment_explain(&d, now, output, sizeof(output)));
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, output, -1, NULL, 0) > 0);
    CHECK(strstr(output, "Unknown:") && strstr(output, "Next check:") && strstr(output, "Window:") && strstr(output, " UTC"));
    CHECK(strstr(output, "truncated") && strstr(output, "unavailable") && !strstr(output, "nan"));
    CHECK(!experiment_explain(&d, now, tiny, sizeof(tiny)));
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tiny, -1, NULL, 0) > 0);
}
static void test_all_fixed_metric_thresholds_and_isolation(void)
{
    Fixture f; ExperimentStats s; ExperimentDecision *d; size_t n; unsigned m; const double limits[4] = {25, 536870912, 10485760, 10485760};
    area = "four metric fixed thresholds / independent missing"; init(&f, 1); CHECK(tick(&f, 0));
    f.rows[0].cpu_percent = 40; f.rows[0].working_set_bytes = UINT64_C(1073741824);
    f.rows[0].io_read_bytes_per_second = f.rows[0].io_write_bytes_per_second = 20971520;
    window(&f); window(&f); window(&f); s = stats(&f); CHECK(s.active == 4 && s.episodes == 4);
    d = decisions(&f, EXPERIMENT_FIXED, &n);
    for (m = 0; m < n; ++m) CHECK(d[m].report.threshold == limits[d[m].metric]);
    experiment_decisions_free(d, n);
    f.rows[0].valid_fields &= ~(unsigned)RESOURCE_VALID_IO_READ; CHECK(tick(&f, 3));
    s = stats(&f); CHECK(s.active == 3 && s.unknown == 1 && s.recovered == 0);
    f.rows[0].valid_fields |= RESOURCE_VALID_IO_READ; f.rows[0].cpu_percent = 0; f.rows[0].working_set_bytes = 0;
    f.rows[0].io_write_bytes_per_second = 0; next_completed_window(&f); s = stats(&f);
    CHECK(s.active == 0 && s.unknown == 1 && s.recovered == 3);
    next_completed_window(&f); next_completed_window(&f); CHECK(stats(&f).active == 1 && stats(&f).episodes == 5);
    done(&f);
}
static double qpc(void)
{
    LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    return (double)n.QuadPart * 1000.0 / (double)f.QuadPart;
}
static int compare_double(const void *a, const void *b)
{ double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static uint64_t file_bytes(const char *file)
{
    WIN32_FILE_ATTRIBUTE_DATA x;
    if (!GetFileAttributesExA(file, GetFileExInfoStandard, &x)) return 0;
    return ((uint64_t)x.nFileSizeHigh << 32) | x.nFileSizeLow;
}
static void sizes(const char *label, const char *file)
{
    char wal[MAX_PATH]; snprintf(wal, sizeof(wal), "%s-wal", file);
    printf("BENCH_FILE %s db=%llu wal=%llu path=%s\n", label, (unsigned long long)file_bytes(file), (unsigned long long)file_bytes(wal), file);
}
static void memory(const char *label)
{
    PROCESS_MEMORY_COUNTERS_EX x;
    memset(&x, 0, sizeof(x)); x.cb = sizeof(x);
    CHECK(GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&x, sizeof(x)));
    printf("BENCH_MEMORY %s working_set=%zu private_bytes=%zu process_peak_working_set=%zu\n", label, (size_t)x.WorkingSetSize, (size_t)x.PrivateUsage, (size_t)x.PeakWorkingSetSize);
}
static void timing(const char *label, double *values, size_t n)
{
    qsort(values, n, sizeof(*values), compare_double);
    printf("BENCH_TIME %s n=%zu p50_ms=%.6f p95_ms=%.6f max_ms=%.6f\n", label, n, values[(n - 1) / 2], values[(n * 95 + 99) / 100 - 1], values[n - 1]);
}
static void benchmark(unsigned count)
{
    enum { SAMPLES = 101 };
    ResourceData *rows = calloc(count, sizeof(*rows));
    const char **paths = calloc(count, sizeof(*paths));
    MonitorMetricReport *reports = calloc((size_t)count * 4, sizeof(*reports));
    ResourceBatch b; MonitorEngine *core = NULL; MonitorSnapshot *s = NULL; Experiment *e = NULL;
    char source[MAX_PATH], side[MAX_PATH], original[MAX_PATH], error[512], label[80];
    uint64_t start = monitor_utc_now() / DAY * DAY + 43200 * TICKS;
    double total[SAMPLES], store[SAMPLES], overhead[SAMPLES]; size_t completed[2] = {0, 0};
    unsigned mode, i, j, m;
    area = "initially-learning performance";
    if (!rows || !paths || !reports) fatal("benchmark allocation");
    memset(&b, 0, sizeof(b)); b.items = rows; b.count = count; b.logical_processor_count = 8;
    for (j = 0; j < count; ++j) {
        rows[j].pid = 20000 + j; rows[j].creation_time_utc_100ns = start - 3600 * TICKS;
        rows[j].process_name_utf8 = "bench.exe";
        paths[j] = "SYNTHETIC:\\review-perf\\bench.exe";
        rows[j].valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
            RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        rows[j].working_set_bytes = 67108864;
        rows[j].observation_interval_seconds = rows[j].cpu_interval_seconds = rows[j].io_interval_seconds = 3;
        rows[j].foreground = RESOURCE_FOREGROUND_UNKNOWN;
    }
    path(original, sizeof(original), "bench-original"); path(source, sizeof(source), "bench-source"); path(side, sizeof(side), "bench-evidence");
    for (mode = 0; mode < 2; ++mode) {
        if (mode == 0) CHECK(monitor_open(original, 1, &core, error, sizeof(error)));
        else CHECK(experiment_create(source, side, 1, &e, error, sizeof(error)));
        if ((!mode && !core) || (mode && !e)) fatal(error);
        snprintf(label, sizeof(label), "instances=%u mode=%s before", count, mode ? "experiment" : "original"); memory(label);
        for (i = 0; i < SAMPLES; ++i) {
            double before;
            b.started_at_utc_100ns = b.finished_at_utc_100ns = start + (uint64_t)i * 3 * TICKS;
            for (j = 0; j < count; ++j) {
                rows[j].measured_at_utc_100ns = b.finished_at_utc_100ns;
                rows[j].cpu_percent = i <= 20 ? 10 : 40;
            }
            before = qpc();
            CHECK(mode ? experiment_ingest(e, &b, paths, &s) : monitor_ingest(core, &b, paths, &s));
            total[i] = qpc() - before;
            if (!s) fatal(mode ? experiment_error(e) : monitor_error(core));
            store[i] = s->store_ms; overhead[i] = total[i] - s->store_ms; completed[mode] += s->completed_windows;
            if (i == SAMPLES - 1) for (j = 0; j < count; ++j) for (m = 0; m < 4; ++m) {
                MonitorMetricReport *r = &s->rows[j].metric[m];
                if (!mode) reports[(size_t)j * 4 + m] = *r;
                else {
                    MonitorMetricReport *old = &reports[(size_t)j * 4 + m];
                    CHECK(r->state == old->state && r->valid_samples == old->valid_samples && r->streak == old->streak);
                    CHECK(r->current == old->current && r->covered_seconds == old->covered_seconds && r->window_duration_seconds == old->window_duration_seconds);
                    CHECK(r->count == old->count && r->days == old->days && r->minutes == old->minutes);
                }
            }
            monitor_snapshot_free(s); s = NULL;
        }
        snprintf(label, sizeof(label), "instances=%u mode=%s total", count, mode ? "experiment" : "original"); timing(label, total, SAMPLES);
        snprintf(label, sizeof(label), "instances=%u mode=%s core-store", count, mode ? "experiment" : "original"); timing(label, store, SAMPLES);
        snprintf(label, sizeof(label), "instances=%u mode=%s outside-core-store", count, mode ? "experiment" : "original"); timing(label, overhead, SAMPLES);
        snprintf(label, sizeof(label), "instances=%u mode=%s after", count, mode ? "experiment" : "original"); memory(label);
        printf("BENCH_QUALITY instances=%u mode=%s completed=%zu expected=%u all_final_reports_equal=%d\n", count, mode ? "experiment" : "original", completed[mode], count * 5, mode ? (failures == 0) : 1);
        if (!mode) sizes("original-open", original);
        else { ExperimentStats z; CHECK(experiment_stats(e, &z)); CHECK(z.windows == (uint64_t)count * 5); sizes("experiment-source-open", source); sizes("evidence-open", side); }
        if (!mode) { monitor_close(core); core = NULL; } else { experiment_close(e); e = NULL; }
    }
    CHECK(completed[0] == completed[1] && completed[0] == (size_t)count * 5);
    free(rows); free(paths); free(reports);
}
int main(int argc, char **argv)
{
    CreateDirectoryA("build-review-prototype", NULL);
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) {
        benchmark(100); benchmark(300);
        printf("RESULT benchmark checks=%u failures=%u\n", checks, failures); return failures ? 1 : 0;
    }
    test_unready_fixed_and_zero(); test_personalized_and_fixed_independence();
    test_missing_resets_immediately(); test_same_name_pid_reuse_absence();
    test_past_future_and_same_cycle(); test_restart_history_replay_export_evaluation(); test_unrecognized_guard();
    test_rollback_and_sidecar_failure(); test_retained_replay_boundary(); test_gap_duplicate_and_late_order(); test_retained_recovery_only();
    test_full_replay_recomputes_quality();
    test_exposed_ids_never_reused();
    test_companion_guard_and_formula_text();
    test_latest_source_quality(); test_repeated_invalid_identity_has_no_fake_gaps();
    test_long_utf8_explanation_boundary();
    test_all_fixed_metric_thresholds_and_isolation();
    printf("RESULT independent review checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
