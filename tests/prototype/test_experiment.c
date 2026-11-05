/* 합성 회차로 동일 완료창 비교, 관측 단절, 사건 종료와 이력·평가·재생 경계를 검증한다.
 * 날짜 진행은 입력 시각이며 실제 장기 사용이나 사용자 효용 평가를 뜻하지 않는다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "experiment.h"
#include "sqlite3.h"

#define TICKS UINT64_C(10000000)
#define DAY (UINT64_C(86400)*TICKS)
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
typedef struct Fixture {
    Experiment *e;
    ResourceBatch batch;
    ResourceData row;
    MonitorSnapshot *snapshot;
    uint64_t now, anchor;
    char source[MAX_PATH], evidence[MAX_PATH], replay[MAX_PATH], csv[MAX_PATH];
} Fixture;
static int64_t scalar_file(const char *file, const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    int64_t v = -1;
    CHECK(sqlite3_open_v2(file, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0); else CHECK(0);
    sqlite3_finalize(s); sqlite3_close(db); return v;
}
static void init(Fixture *f)
{
    char temp[MAX_PATH], error[512];
    DWORD pid = GetCurrentProcessId();
    memset(f, 0, sizeof(*f)); GetTempPathA(MAX_PATH, temp);
    snprintf(f->source, sizeof(f->source), "%sexperiment-basic-%lu-source.sqlite", temp, (unsigned long)pid);
    snprintf(f->evidence, sizeof(f->evidence), "%sexperiment-basic-%lu-events.sqlite", temp, (unsigned long)pid);
    snprintf(f->replay, sizeof(f->replay), "%sexperiment-basic-%lu-replay.sqlite", temp, (unsigned long)pid);
    snprintf(f->csv, sizeof(f->csv), "%sexperiment-basic-%lu.csv", temp, (unsigned long)pid);
    CHECK(experiment_create(f->source, f->evidence, 1, &f->e, error, sizeof(error)));
    if (!f->e) { printf("create: %s\n", error); exit(1); }
    f->anchor = monitor_utc_now() / DAY * DAY - 4 * DAY + 12 * 3600 * TICKS;
    f->now = f->anchor;
    f->batch.items = &f->row; f->batch.count = 1; f->batch.logical_processor_count = 8;
    f->row.pid = 4242; f->row.creation_time_utc_100ns = f->anchor - TICKS;
    f->row.process_name_utf8 = "same.exe";
    f->row.valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
        RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
    f->row.cpu_percent = 2; f->row.working_set_bytes = UINT64_C(67108864);
    f->row.observation_interval_seconds = f->row.cpu_interval_seconds = f->row.io_interval_seconds = 3;
}
static int tick(Fixture *f, double dt)
{
    const char *paths[1] = {"SYNTHETIC:\\A\\same.exe"};
    f->now += (uint64_t)(dt * (double)TICKS); f->row.measured_at_utc_100ns = f->now;
    f->batch.started_at_utc_100ns = f->batch.finished_at_utc_100ns = f->now;
    monitor_snapshot_free(f->snapshot); f->snapshot = NULL;
    return experiment_ingest(f->e, &f->batch, paths, &f->snapshot);
}
static void window(Fixture *f)
{ unsigned n; for (n = 0; n < 20; ++n) CHECK(tick(f, 3)); CHECK(f->snapshot && f->snapshot->completed_windows == 1); }
static ExperimentDecision latest_cpu(Fixture *f, ExperimentMethod method)
{
    ExperimentDecision *d = NULL, result;
    size_t count, i;
    memset(&result, 0, sizeof(result));
    CHECK(experiment_decisions(f->e, method, 4, 0, &d, &count));
    for (i = 0; i < count; ++i) if (d[i].metric == MONITOR_CPU) { result = d[i]; result.name = NULL; result.path = NULL; break; }
    CHECK(i < count); experiment_decisions_free(d, count); return result;
}
static void run(void)
{
    Fixture f;
    Experiment *other = NULL;
    ExperimentStats stats, replay_stats;
    ExperimentEpisode *episodes = NULL;
    ExperimentDecision *decision = NULL;
    ExperimentDecision personal, fixed;
    ExperimentEvaluation v;
    size_t count;
    unsigned date, n;
    int64_t source_metrics, episode_id, recovery_decision;
    uint64_t gaps_before;
    char error[512], text[4096], held[MAX_PATH];
    init(&f);
    CHECK(!experiment_create(f.source, f.evidence, 1, &other, error, sizeof(error)) && !other);
    CHECK(tick(&f, 0)); window(&f);
    personal = latest_cpu(&f, EXPERIMENT_PERSONALIZED); fixed = latest_cpu(&f, EXPERIMENT_FIXED);
    CHECK(personal.report.state == MONITOR_LEARNING && fixed.report.state == MONITOR_NORMAL);
    CHECK(fixed.window_id == personal.window_id && fixed.report.current == personal.report.current);
    CHECK(experiment_stats(f.e, &stats)); CHECK(stats.latest_source_available && stats.latest_source_rows == 1 && stats.latest_source_metric_missing[0] == 0);
    gaps_before = stats.gaps;
    f.row.valid_fields &= ~(RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU);
    CHECK(tick(&f, 3)); CHECK(experiment_stats(f.e, &stats));
    CHECK(stats.latest_source_available && stats.latest_source_identity_unavailable == 1 && stats.latest_source_path_unavailable == 1 && stats.latest_source_metric_missing[0] == 1);
    CHECK(stats.latest_source_metric_missing[1] == 0);
    CHECK(stats.gaps == gaps_before + 1); /* 유효 신원에서 미확인 신원으로 바뀌면 기존 관측 연결 한 번을 끊는다. */
    gaps_before = stats.gaps;
    for (n = 0; n < 10; ++n) CHECK(tick(&f, 3));
    CHECK(experiment_stats(f.e, &stats)); CHECK(stats.gaps == gaps_before); /* 미확인 신원의 반복을 매번 인스턴스 부재로 처리하지 않는다. */
    f.row.valid_fields |= RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU;
    CHECK(experiment_reset(f.e, f.now, "diagnostic fixture boundary")); CHECK(tick(&f, 0));
    for (date = 0; date < 3; ++date) {
        if (date) { f.now = f.anchor + date * DAY; CHECK(experiment_reset(f.e, f.now, "synthetic date jump")); CHECK(tick(&f, 0)); }
        for (n = 0; n < 22; ++n) window(&f);
    }
    personal = latest_cpu(&f, EXPERIMENT_PERSONALIZED);
    CHECK(personal.report.state == MONITOR_NORMAL && personal.report.count >= 60 && personal.report.days == 3);
    CHECK(personal.report.threshold == 7);
    f.row.cpu_percent = 12;
    window(&f); window(&f); window(&f); window(&f);
    CHECK(experiment_stats(f.e, &stats)); CHECK(stats.episodes == 1 && stats.active == 1);
    CHECK(experiment_episodes(f.e, -1, 20, 0, &episodes, &count)); CHECK(count == 1);
    CHECK(episodes[0].qualifying_windows == 4 && episodes[0].method == EXPERIMENT_PERSONALIZED);
    episode_id = episodes[0].id; experiment_episodes_free(episodes, count);
    personal = latest_cpu(&f, EXPERIMENT_PERSONALIZED); fixed = latest_cpu(&f, EXPERIMENT_FIXED);
    CHECK(personal.report.state == MONITOR_CANDIDATE && fixed.report.state == MONITOR_NORMAL);
    CHECK(experiment_get_decision(f.e, personal.id, &decision));
    CHECK(experiment_explain(decision, f.now + DAY, text, sizeof(text)));
    CHECK(strstr(text, "Target:") && strstr(text, "Window:") && strstr(text, "PID 4242") && strstr(text, "Historical/stale"));
    CHECK(strstr(text, "Absolute difference") && strstr(text, "Unknown:") && strstr(text, "Next check:"));
    {
        char long_name[1501], long_path[3001], tiny[32];
        char *original_name = decision->name, *original_path = decision->path;
        unsigned k;
        for (k = 0; k < 1500; k += 3) memcpy(long_name + k, "\xea\xb0\x80", 3);
        for (k = 0; k < 3000; k += 3) memcpy(long_path + k, "\xea\xb0\x80", 3);
        long_name[1500] = 0; long_path[3000] = 0; decision->name = long_name; decision->path = long_path;
        CHECK(experiment_explain(decision, f.now, text, sizeof(text)));
        CHECK(strstr(text, "truncated; full metadata in export") && strstr(text, "Unknown:") && strstr(text, "Next check:") && strstr(text, "Window:"));
        CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0) > 0);
        CHECK(!experiment_explain(decision, f.now, tiny, sizeof(tiny)));
        decision->name = original_name; decision->path = original_path;
    }
    experiment_decisions_free(decision, 1); decision = NULL;
    f.row.cpu_percent = 0; window(&f);
    CHECK(experiment_episodes(f.e, -1, 20, 0, &episodes, &count)); CHECK(count == 1 && episodes[0].state == EXPERIMENT_RECOVERED);
    recovery_decision = episodes[0].last_decision_id; experiment_episodes_free(episodes, count);
    CHECK(experiment_get_decision(f.e, recovery_decision, &decision)); CHECK(decision->report.current == 0 && decision->report.state == MONITOR_NORMAL);
    experiment_decisions_free(decision, 1); decision = NULL;
    f.row.cpu_percent = 40; window(&f); window(&f); window(&f);
    CHECK(experiment_stats(f.e, &stats)); CHECK(stats.active == 2 && stats.episodes == 3);
    f.row.valid_fields &= ~RESOURCE_VALID_CPU; CHECK(tick(&f, 3));
    CHECK(experiment_stats(f.e, &stats)); CHECK(stats.active == 0 && stats.unknown == 2);
    f.row.valid_fields |= RESOURCE_VALID_CPU;
    for (n = 0; n < 19; ++n) CHECK(tick(&f, 3));
    personal = latest_cpu(&f, EXPERIMENT_PERSONALIZED); fixed = latest_cpu(&f, EXPERIMENT_FIXED);
    CHECK(personal.report.streak == 1 && fixed.report.streak == 1 && !fixed.continuity);
    CHECK(experiment_finish(f.e, f.now));
    source_metrics = scalar_file(f.source, "SELECT count(*) FROM aggregate_metric");
    memset(&v, 0, sizeof(v)); v.episode_id = episode_id; v.decision_id = recovery_decision; v.trial_id = "basic-held-out"; v.order_id = "AB";
    v.judgment = EXPERIMENT_UNSURE; v.understood = v.target_correct = v.time_correct = v.metric_correct = v.next_check = -1;
    v.elapsed_seconds = -1; v.note = "synthetic contract record; not a human utility result";
    CHECK(experiment_evaluate(f.e, &v));
    CHECK(scalar_file(f.evidence, "SELECT elapsed IS NULL FROM evaluation LIMIT 1") == 1);
    CHECK(scalar_file(f.source, "SELECT count(*) FROM aggregate_metric") == source_metrics);
    v.trial_id = ""; CHECK(!experiment_evaluate(f.e, &v));
    CHECK(experiment_export(f.e, f.csv)); CHECK(!experiment_export(f.e, f.csv));
    CHECK(experiment_stats(f.e, &stats));
    experiment_close(f.e); f.e = NULL;
    snprintf(held, sizeof(held), "%s.held", f.source);
    CHECK(GetFileAttributesA(held) == INVALID_FILE_ATTRIBUTES); CHECK(MoveFileA(f.source, held));
    CHECK(experiment_open_history(f.evidence, &other, error, sizeof(error)));
    CHECK(experiment_stats(other, &replay_stats)); CHECK(!replay_stats.source_available && !replay_stats.latest_source_available);
    experiment_close(other); other = NULL; CHECK(MoveFileA(held, f.source));
    CHECK(experiment_replay(f.evidence, f.replay, error, sizeof(error)));
    CHECK(experiment_open_history(f.replay, &other, error, sizeof(error)));
    CHECK(experiment_stats(other, &replay_stats));
    CHECK(replay_stats.replay_only && replay_stats.synthetic && replay_stats.replay_comparator_recomputed);
    CHECK(replay_stats.windows == stats.windows && replay_stats.decisions == stats.decisions && replay_stats.episodes == stats.episodes);
    CHECK(replay_stats.active == stats.active && replay_stats.recovered == stats.recovered && replay_stats.unknown == stats.unknown);
    experiment_close(other); other = NULL;
    CHECK(!experiment_replay(f.evidence, f.replay, error, sizeof(error)));
    CHECK(experiment_resume(f.evidence, &f.e, error, sizeof(error))); CHECK(experiment_finish(f.e, f.now)); experiment_close(f.e);
    monitor_snapshot_free(f.snapshot);
    CHECK(DeleteFileA(f.source)); CHECK(DeleteFileA(f.evidence)); CHECK(DeleteFileA(f.replay)); CHECK(DeleteFileA(f.csv));
}
int main(void)
{
    run(); printf("experiment tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
