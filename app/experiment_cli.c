/* 실험용 수집·합성 시연·이력·평가 명령을 연결하는 콘솔 진입점이다.
 * 실제 수집 루프와 회차 사이 대기는 이 앱이 소유한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "experiment.h"

#define TICKS UINT64_C(10000000)
#define DAY (UINT64_C(86400) * TICKS)
static HANDLE stop_event;

typedef struct Options {
    char source[32768], db[32768], out[32768], note[1024], trial[128], order[128];
    unsigned count, interval, limit;
    int resume, windows, method;
    int64_t decision, episode;
    ExperimentJudgment judgment;
} Options;

static BOOL WINAPI on_control(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        if (stop_event) SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}
static int utf8(const wchar_t *value, char *out, size_t size)
{
    return size <= INT_MAX && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value, -1, out, (int)size, NULL, NULL) != 0;
}
static int integer(const wchar_t *value, uint64_t max, uint64_t *out)
{
    wchar_t *end;
    unsigned long long parsed;
    if (!value[0] || value[0] == L'-' || value[0] == L'+') return 0;
    errno = 0; parsed = wcstoull(value, &end, 10);
    if (errno || *end || parsed > max) return 0;
    *out = (uint64_t)parsed; return 1;
}
static void usage(void)
{
    puts("prototype_cli demo --source NEW_MONITOR.sqlite --db NEW_EVENTS.sqlite\n"
        "prototype_cli live --source NEW_MONITOR.sqlite --db NEW_EVENTS.sqlite [--count N] [--interval-ms N]\n"
        "prototype_cli live --resume --db EVENTS.sqlite [--count N] [--interval-ms N]\n"
        "prototype_cli list --db EVENTS.sqlite [--windows] [--method personalized|fixed|all] [--limit N]\n"
        "prototype_cli detail --db EVENTS.sqlite --decision ID\n"
        "prototype_cli evaluate --db EVENTS.sqlite --decision ID [--episode ID] --judgment useful|known|unnecessary|unsure\n"
        "    [--trial ID] [--order ID] [--note TEXT]   (elapsed time remains unmeasured)\n"
        "prototype_cli export --db EVENTS.sqlite --out NEW.csv\n"
        "prototype_cli replay --db EVENTS.sqlite --out NEW_REPLAY.sqlite\n"
        "All files remain local. No output overwrite. Demo is accelerated synthetic input, not real learning.\n"
        "Live defaults: unlimited rounds, 3000ms wait AFTER each round; Ctrl+C stops cleanly.");
}
static int parse(int argc, wchar_t **argv, Options *o)
{
    int i;
    memset(o, 0, sizeof(*o)); o->interval = 3000; o->limit = 100; o->method = -1;
    snprintf(o->trial, sizeof(o->trial), "ad-hoc"); snprintf(o->order, sizeof(o->order), "unassigned");
    for (i = 2; i < argc; ++i) {
        uint64_t number;
        if (!wcscmp(argv[i], L"--resume")) o->resume = 1;
        else if (!wcscmp(argv[i], L"--windows")) o->windows = 1;
        else if (i + 1 >= argc) return 0;
        else if (!wcscmp(argv[i], L"--source")) { if (!utf8(argv[++i], o->source, sizeof(o->source))) return 0; }
        else if (!wcscmp(argv[i], L"--db")) { if (!utf8(argv[++i], o->db, sizeof(o->db))) return 0; }
        else if (!wcscmp(argv[i], L"--out")) { if (!utf8(argv[++i], o->out, sizeof(o->out))) return 0; }
        else if (!wcscmp(argv[i], L"--note")) { if (!utf8(argv[++i], o->note, sizeof(o->note))) return 0; }
        else if (!wcscmp(argv[i], L"--trial")) { if (!utf8(argv[++i], o->trial, sizeof(o->trial))) return 0; }
        else if (!wcscmp(argv[i], L"--order")) { if (!utf8(argv[++i], o->order, sizeof(o->order))) return 0; }
        else if (!wcscmp(argv[i], L"--count")) {
            if (!integer(argv[++i], UINT_MAX, &number)) return 0; o->count = (unsigned)number;
        } else if (!wcscmp(argv[i], L"--interval-ms")) {
            if (!integer(argv[++i], 60000, &number) || !number) return 0; o->interval = (unsigned)number;
        } else if (!wcscmp(argv[i], L"--limit")) {
            if (!integer(argv[++i], 10000, &number) || !number) return 0; o->limit = (unsigned)number;
        } else if (!wcscmp(argv[i], L"--decision")) {
            if (!integer(argv[++i], INT64_MAX, &number) || !number) return 0; o->decision = (int64_t)number;
        } else if (!wcscmp(argv[i], L"--episode")) {
            if (!integer(argv[++i], INT64_MAX, &number) || !number) return 0; o->episode = (int64_t)number;
        } else if (!wcscmp(argv[i], L"--method")) {
            ++i;
            if (!wcscmp(argv[i], L"personalized")) o->method = EXPERIMENT_PERSONALIZED;
            else if (!wcscmp(argv[i], L"fixed")) o->method = EXPERIMENT_FIXED;
            else if (!wcscmp(argv[i], L"all")) o->method = -1;
            else return 0;
        } else if (!wcscmp(argv[i], L"--judgment")) {
            ++i;
            if (!wcscmp(argv[i], L"useful")) o->judgment = EXPERIMENT_USEFUL;
            else if (!wcscmp(argv[i], L"known")) o->judgment = EXPERIMENT_KNOWN;
            else if (!wcscmp(argv[i], L"unnecessary")) o->judgment = EXPERIMENT_UNNECESSARY;
            else if (!wcscmp(argv[i], L"unsure")) o->judgment = EXPERIMENT_UNSURE;
            else return 0;
        } else return 0;
    }
    return o->db[0] != 0;
}
static void stats(Experiment *e)
{
    ExperimentStats s;
    unsigned method, state;
    if (!experiment_stats(e, &s)) { fprintf(stderr, "stats: %s\n", experiment_error(e)); return; }
    printf("origin=%s policy=%s template=%s source_available=%d replay_only=%d replay_comparator=%s\n"
        "windows=%llu decisions=%llu episodes=%llu active=%llu recovered=%llu unknown=%llu evaluations=%llu gaps=%llu\n",
        s.synthetic ? "SYNTHETIC" : "real", EXPERIMENT_POLICY_VERSION, EXPERIMENT_TEMPLATE_VERSION,
        s.source_available, s.replay_only, !s.replay_only ? "not-replay" : s.replay_comparator_recomputed ?
            "fixed-recomputed-personalized-recorded" : "expired-prefix-projection-only",
        (unsigned long long)s.windows, (unsigned long long)s.decisions,
        (unsigned long long)s.episodes, (unsigned long long)s.active, (unsigned long long)s.recovered,
        (unsigned long long)s.unknown, (unsigned long long)s.evaluations, (unsigned long long)s.gaps);
    if (s.latest_source_available) printf("latest_source_raw available=1 utc_100ns=%llu status=%d raw_rows=%llu expected_rows=%llu "
        "identity_unavailable=%llu path_unavailable=%llu metric_missing_cpu=%llu ram=%llu read=%llu write=%llu (counts overlap; not completed-window states)\n",
        (unsigned long long)s.latest_source_utc, s.latest_source_status, (unsigned long long)s.latest_source_raw_rows,
        (unsigned long long)s.latest_source_rows, (unsigned long long)s.latest_source_identity_unavailable,
        (unsigned long long)s.latest_source_path_unavailable, (unsigned long long)s.latest_source_metric_missing[0],
        (unsigned long long)s.latest_source_metric_missing[1], (unsigned long long)s.latest_source_metric_missing[2],
        (unsigned long long)s.latest_source_metric_missing[3]);
    else puts("latest_source_raw available=0 missing_counts=NA (source absent/expired/incomplete or replay; retained comparisons are separate)");
    if (!s.windows) puts("comparison_windows=none_completed_yet (not normal; latest raw diagnostics are separate)");
    for (method = 0; method < 2; ++method) for (state = 0; state < 5; ++state)
        printf("availability method=%s state=%s metric_windows=%llu metric_window_seconds=%.3f\n",
            experiment_method_name((ExperimentMethod)method), monitor_state_name((MonitorState)state),
            (unsigned long long)s.states[method][state], s.window_seconds[method][state]);
}
static int list_history(Experiment *e, const Options *o)
{
    size_t i, count = 0;
    if (o->windows) {
        int method;
        for (method = 0; method < 2; ++method) if (o->method < 0 || o->method == method) {
            ExperimentDecision *d = NULL;
            if (!experiment_decisions(e, (ExperimentMethod)method, o->limit, 0, &d, &count)) return 0;
            for (i = 0; i < count; ++i) printf("decision=%lld method=%s pid=%lu creation=%llu metric=%s state=%s "
                "start=%llu end=%llu current=%.6g threshold=%.6g streak=%u episode=%lld name=%s\n",
                (long long)d[i].id, experiment_method_name(d[i].method), (unsigned long)d[i].pid,
                (unsigned long long)d[i].creation, monitor_metric_name(d[i].metric), monitor_state_name(d[i].report.state),
                (unsigned long long)d[i].start_utc, (unsigned long long)d[i].end_utc,
                d[i].report.current, d[i].report.threshold, d[i].report.streak, (long long)d[i].episode_id, d[i].name);
            experiment_decisions_free(d, count);
        }
    } else {
        ExperimentEpisode *p = NULL;
        if (!experiment_episodes(e, o->method, o->limit, 0, &p, &count)) return 0;
        for (i = 0; i < count; ++i) printf("episode=%lld method=%s pid=%lu creation=%llu metric=%s state=%s "
            "first=%llu activated=%llu last=%llu ended=%llu windows=%u detail_decision=%lld name=%s\n",
            (long long)p[i].id, experiment_method_name(p[i].method), (unsigned long)p[i].pid,
            (unsigned long long)p[i].creation, monitor_metric_name(p[i].metric), experiment_episode_state_name(p[i].state),
            (unsigned long long)p[i].first_utc, (unsigned long long)p[i].activated_utc, (unsigned long long)p[i].last_utc,
            (unsigned long long)p[i].ended_utc, p[i].qualifying_windows, (long long)p[i].last_decision_id, p[i].name);
        experiment_episodes_free(p, count);
    }
    puts("List is bounded; export includes retained history. Times above are UTC FILETIME 100ns ticks.");
    return 1;
}
/* 회차별 batch와 snapshot을 사용 후 해제하고 정지 요청 시 관측 종료를 기록한다.
 * QPC는 실제 처리 비용 측정에, UTC는 저장 시각에 사용한다. */
static int live(Experiment *e, const Options *o)
{
    ResourceCollector *collector = NULL;
    LARGE_INTEGER frequency, total_begin, total_end, collect_begin, collected, ingested, finish_begin, finish_end;
    FILETIME created, exited, kernel_begin, user_begin, kernel_end, user_end;
    IO_COUNTERS io_begin, io_end;
    int cpu_begin_valid, cpu_end_valid, io_begin_valid, io_end_valid;
    DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    uint32_t error = 0;
    unsigned round = 0;
    int ok = 1;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&total_begin);
    cpu_begin_valid = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_begin, &user_begin) != 0;
    io_begin_valid = GetProcessIoCounters(GetCurrentProcess(), &io_begin) != 0;
    if (resource_collector_create(NULL, &collector, &error) != RESOURCE_OK) {
        fprintf(stderr, "collector create failed Win32=%lu\n", (unsigned long)error); return 0;
    }
    printf("REAL local collection; no command lines/window titles/files captured. Wait-after-round=%ums\n", o->interval);
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        ResourceBatch batch = {0};
        MonitorSnapshot *snapshot = NULL;
        PROCESS_MEMORY_COUNTERS_EX memory;
        int memory_valid;
        double total_ms;
        QueryPerformanceCounter(&collect_begin);
        if (resource_collector_collect(collector, &batch, &error) != RESOURCE_OK) {
            fprintf(stderr, "collector failure Win32=%lu; continuity invalidated\n", (unsigned long)error);
            (void)experiment_reset(e, monitor_utc_now(), "collector failure"); ok = 0; break;
        }
        QueryPerformanceCounter(&collected);
        ok = experiment_ingest(e, &batch, NULL, &snapshot);
        QueryPerformanceCounter(&ingested);
        resource_batch_free(&batch);
        if (!ok) { monitor_snapshot_free(snapshot); break; }
        ++round;
        total_ms = (double)(ingested.QuadPart - collected.QuadPart) * 1000 / (double)frequency.QuadPart;
        memset(&memory, 0, sizeof(memory)); memory.cb = sizeof(memory);
        memory_valid = GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory)) != 0;
        printf("round=%u rows=%zu completed_windows=%zu collect_ms=%.3f ingest_total_ms=%.3f core_store_ms=%.3f sidecar_overhead_ms=%.3f ", round,
            snapshot->count, snapshot->completed_windows,
            (double)(collected.QuadPart - collect_begin.QuadPart) * 1000 / (double)frequency.QuadPart,
            total_ms, snapshot->store_ms, total_ms - snapshot->store_ms);
        if (memory_valid) printf("monitor_working_set_bytes=%llu monitor_private_bytes=%llu ",
            (unsigned long long)memory.WorkingSetSize, (unsigned long long)memory.PrivateUsage);
        else fputs("monitor_working_set_bytes=NA monitor_private_bytes=NA ", stdout);
        printf("%s\n", snapshot->status);
        monitor_snapshot_free(snapshot); fflush(stdout);
        if (o->count && round >= o->count) break;
        if (WaitForSingleObject(stop_event, o->interval) == WAIT_FAILED) { ok = 0; break; }
    }
    QueryPerformanceCounter(&finish_begin);
    resource_collector_destroy(collector);
    if (!experiment_finish(e, monitor_utc_now())) ok = 0;
    QueryPerformanceCounter(&finish_end); QueryPerformanceCounter(&total_end);
    cpu_end_valid = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_end, &user_end) != 0;
    io_end_valid = GetProcessIoCounters(GetCurrentProcess(), &io_end) != 0;
    {
        double wall = (double)(total_end.QuadPart - total_begin.QuadPart) / (double)frequency.QuadPart;
        printf("live_cost rounds=%u scope=collector-create-through-finish wall_seconds=%.6f graceful_finish_ms=%.3f ", round,
            wall, (double)(finish_end.QuadPart - finish_begin.QuadPart) * 1000 / (double)frequency.QuadPart);
        if (cpu_begin_valid && cpu_end_valid) {
            ULARGE_INTEGER kb, ub, ke, ue;
            double cpu;
            kb.LowPart = kernel_begin.dwLowDateTime; kb.HighPart = kernel_begin.dwHighDateTime;
            ub.LowPart = user_begin.dwLowDateTime; ub.HighPart = user_begin.dwHighDateTime;
            ke.LowPart = kernel_end.dwLowDateTime; ke.HighPart = kernel_end.dwHighDateTime;
            ue.LowPart = user_end.dwLowDateTime; ue.HighPart = user_end.dwHighDateTime;
            cpu = (double)((ke.QuadPart - kb.QuadPart) + (ue.QuadPart - ub.QuadPart)) / (double)TICKS;
            printf("process_cpu_seconds=%.6f ", cpu);
            if (logical && wall > 0) printf("process_cpu_percent_system=%.6f ", cpu * 100 / (wall * logical));
            else fputs("process_cpu_percent_system=NA ", stdout);
        } else fputs("process_cpu_seconds=NA process_cpu_percent_system=NA ", stdout);
        if (io_begin_valid && io_end_valid && io_end.ReadTransferCount >= io_begin.ReadTransferCount &&
            io_end.WriteTransferCount >= io_begin.WriteTransferCount)
            printf("process_read_bytes=%llu process_write_bytes=%llu\n",
                (unsigned long long)(io_end.ReadTransferCount - io_begin.ReadTransferCount),
                (unsigned long long)(io_end.WriteTransferCount - io_begin.WriteTransferCount));
        else fputs("process_read_bytes=NA process_write_bytes=NA\n", stdout);
    }
    return ok;
}

typedef struct Demo {
    Experiment *experiment;
    uint64_t now, creation[2];
    unsigned rounds;
} Demo;
static int demo_sample(Demo *d, double cpu, int cpu_valid)
{
    ResourceData rows[2];
    ResourceBatch batch;
    MonitorSnapshot *snapshot = NULL;
    const char *paths[2] = { "SYNTHETIC:\\prototype\\relative\\fixture.exe", "SYNTHETIC:\\prototype\\steady-high\\fixture.exe" };
    unsigned i;
    memset(rows, 0, sizeof(rows)); memset(&batch, 0, sizeof(batch));
    for (i = 0; i < 2; ++i) {
        ResourceData *r = &rows[i];
        r->pid = 4242 + i; r->creation_time_utc_100ns = d->creation[i];
        r->process_name_utf8 = "SYNTHETIC fixture.exe"; r->measured_at_utc_100ns = d->now;
        r->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU |
            RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE |
            RESOURCE_VALID_FOREGROUND | RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->cpu_percent = i ? 35 : cpu; r->working_set_bytes = UINT64_C(67108864);
        r->io_read_bytes_per_second = r->io_write_bytes_per_second = 0;
        r->observation_interval_seconds = r->cpu_interval_seconds = r->io_interval_seconds = 3;
        r->foreground = RESOURCE_BACKGROUND;
        if (!i && !cpu_valid) { r->valid_fields &= ~RESOURCE_VALID_CPU; r->cpu_status = RESOURCE_DELTA_QUERY_FAILED; }
    }
    batch.items = rows; batch.count = 2; batch.started_at_utc_100ns = batch.finished_at_utc_100ns = d->now;
    batch.foreground_observed_at_utc_100ns = d->now; batch.logical_processor_count = 8;
    if (!experiment_ingest(d->experiment, &batch, paths, &snapshot)) return 0;
    monitor_snapshot_free(snapshot); ++d->rounds; d->now += 3 * TICKS; return 1;
}
static int demo_windows(Demo *d, unsigned count, double cpu, int valid)
{
    unsigned i;
    for (i = 0; i < count * 20; ++i) if (!demo_sample(d, cpu, valid)) return 0;
    return 1;
}
/* 합성 시각을 세 UTC 날짜로 진행시켜 비교 준비 조건과 변화 사례를 시연한다.
 * 실제 다일 학습 효용이나 사람의 평가 결과를 생성하는 경로는 아니다. */
static int demo(Experiment *e)
{
    SYSTEMTIME st = {0};
    FILETIME ft;
    Demo d;
    uint64_t base;
    unsigned day;
    st.wYear = 2026; st.wMonth = 10; st.wDay = 1; st.wHour = 8;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    base = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    memset(&d, 0, sizeof(d)); d.experiment = e;
    puts("SYNTHETIC accelerated timestamps: three UTC dates, 24 completed windows/date/path; no gate bypass.\n"
        "Two same-name/different-path instances: usual CPU 2% and 35%; valid I/O zero is preserved.");
    for (day = 0; day < 3; ++day) {
        d.now = base + day * DAY; d.creation[0] = d.now - TICKS; d.creation[1] = d.now - 2 * TICKS;
        if (day && !experiment_reset(e, d.now, "SYNTHETIC next UTC date; separate instance")) return 0;
        if (!demo_sample(&d, 2, 1) || !demo_windows(&d, 24, 2, 1)) return 0;
    }
    puts("SYNTHETIC scenarios: valid zero; transient high; personal-only relative change; recovery; shared high; missing CPU; PID reuse; long gap.");
    if (!demo_windows(&d, 2, 0, 1) || !demo_windows(&d, 1, 40, 1) || !demo_windows(&d, 1, 2, 1) ||
        !demo_windows(&d, 3, 12, 1) || !demo_windows(&d, 1, 2, 1) || !demo_windows(&d, 3, 40, 1) ||
        !demo_windows(&d, 1, 0, 0) || !demo_windows(&d, 1, 2, 1)) return 0;
    d.creation[0] = d.now - TICKS;
    if (!demo_sample(&d, 40, 1) || !demo_windows(&d, 3, 40, 1)) return 0;
    d.now += 30 * TICKS;
    if (!experiment_reset(e, d.now, "SYNTHETIC long gap, not observed normal") ||
        !demo_sample(&d, 2, 1) || !demo_windows(&d, 1, 2, 1) || !experiment_finish(e, d.now)) return 0;
    printf("Synthetic input rounds=%u; actual wall clock is not multi-day learning. No evaluation labels seeded.\n", d.rounds);
    return 1;
}
int wmain(int argc, wchar_t **argv)
{
    Options o;
    Experiment *e = NULL;
    char error[512] = {0};
    int ok = 0;
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2 || !parse(argc, argv, &o)) { usage(); return 2; }
    if (!wcscmp(argv[1], L"replay")) {
        if (!o.out[0]) { usage(); return 2; }
        ok = experiment_replay(o.db, o.out, error, sizeof(error));
        if (!ok) fprintf(stderr, "replay failed: %s\n", error);
        else puts("Replay completed from retained comparison evidence and continuity; Baseline was not refitted.");
        return ok ? 0 : 1;
    }
    if (!wcscmp(argv[1], L"demo") || !wcscmp(argv[1], L"live")) {
        int synthetic = !wcscmp(argv[1], L"demo");
        if (synthetic && o.resume) { usage(); return 2; }
        if (o.resume) {
            Experiment *history = NULL;
            ExperimentStats existing;
            /* resume이 원본 세션을 만들기 전에 실제·합성·replay 모드 불일치를 거부한다. */
            if (!experiment_open_history(o.db, &history, error, sizeof(error))) {
                fprintf(stderr, "preflight failed: %s\n", error); return 1;
            }
            ok = experiment_stats(history, &existing); experiment_close(history);
            if (!ok || existing.synthetic || existing.replay_only) {
                fputs("Live resume requires a real prototype pair; synthetic/replay history was opened read-only and refused.\n", stderr); return 1;
            }
            ok = experiment_resume(o.db, &e, error, sizeof(error));
        }
        else if (o.source[0]) ok = experiment_create(o.source, o.db, synthetic, &e, error, sizeof(error));
        if (!ok) { fprintf(stderr, "open failed: %s\n", error[0] ? error : "--source and --db must name new files"); return 1; }
        if (!synthetic) {
            ExperimentStats existing;
            if (!experiment_stats(e, &existing) || existing.synthetic || existing.replay_only) {
                fputs("Live collection requires a real prototype pair; synthetic/replay history cannot be resumed as real.\n", stderr);
                experiment_close(e); return 1;
            }
        }
        stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!stop_event) { experiment_close(e); return 1; }
        SetConsoleCtrlHandler(on_control, TRUE);
        ok = synthetic ? demo(e) : live(e, &o);
        SetConsoleCtrlHandler(on_control, FALSE); CloseHandle(stop_event); stop_event = NULL;
    } else {
        if (!wcscmp(argv[1], L"evaluate")) ok = experiment_open_review(o.db, &e, error, sizeof(error));
        else ok = experiment_open_history(o.db, &e, error, sizeof(error));
        if (!ok) { fprintf(stderr, "open failed: %s\n", error); return 1; }
        if (!wcscmp(argv[1], L"list")) ok = list_history(e, &o);
        else if (!wcscmp(argv[1], L"detail") && o.decision) {
            ExperimentDecision *d = NULL;
            ok = experiment_get_decision(e, o.decision, &d);
            if (ok) { char explanation[16384]; ok = experiment_explain(d, monitor_utc_now(), explanation, sizeof(explanation)); if (ok) puts(explanation); }
            experiment_decisions_free(d, d ? 1 : 0);
        } else if (!wcscmp(argv[1], L"export") && o.out[0]) ok = experiment_export(e, o.out);
        else if (!wcscmp(argv[1], L"evaluate") && o.decision && o.judgment != EXPERIMENT_UNRATED) {
            ExperimentEvaluation v;
            memset(&v, 0, sizeof(v)); v.decision_id = o.decision; v.episode_id = o.episode;
            v.trial_id = o.trial; v.order_id = o.order; v.judgment = o.judgment; v.note = o.note;
            v.understood = v.target_correct = v.time_correct = v.metric_correct = v.next_check = -1;
            v.elapsed_seconds = -1; ok = experiment_evaluate(e, &v);
            if (ok) puts("Evaluation saved separately. Elapsed time unmeasured; comparison evidence and Baseline unchanged.");
        } else { usage(); experiment_close(e); return 2; }
    }
    if (!ok) fprintf(stderr, "operation failed: %s\n", experiment_error(e));
    stats(e); experiment_close(e); return ok ? 0 : 1;
}
