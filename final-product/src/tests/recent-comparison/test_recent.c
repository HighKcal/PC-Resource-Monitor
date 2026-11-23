/* 고정 합성 입력과 새 시험 DB로 최근 비교의 엔진·저장·WPF bridge 경계를 검사한다.
 * test_engine은 최신 완료창과 별도 과거 4/5개 조건, test_boundaries는 선택 범위,
 * test_engine_long_transition은 장기 기준 우선과 현재 결측의 구분을 확인한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "../support/monitor_fixture.h"
#include "../../engine/monitor_recent.h"
#include "../../engine/monitor_source.h"
#include "../../storage/monitor_recent_store.h"
#include "../../wpf/native/bridge.h"
#define SECOND UINT64_C(10000000)
#define MINUTE (60*SECOND)
static unsigned checks, failures, serial, selects;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
typedef struct Fixture {
    MonitorEngine *engine;
    MonitorSnapshot *snapshot;
    ResourceData row;
    ResourceBatch batch;
    const char *path;
    uint64_t now;
    char file[MAX_PATH];
} Fixture;
static void execute(Fixture *f, const char *format, ...)
{
    char text[4096], *error = NULL;
    va_list args;
    va_start(args, format); vsnprintf(text, sizeof(text), format, args); va_end(args);
    if (sqlite3_exec(monitor_fixture_db(f->engine), text, NULL, NULL, &error) != SQLITE_OK) {
        printf("fixture SQL: %s\n", error ? error : "unknown"); CHECK(0);
    }
    sqlite3_free(error);
}
static int64_t scalar(Fixture *f, const char *text)
{
    sqlite3_stmt *s = NULL;
    int64_t n = 0;
    CHECK(sqlite3_prepare_v2(monitor_fixture_db(f->engine), text, -1, &s, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(s) == SQLITE_ROW); n = sqlite3_column_int64(s, 0); sqlite3_finalize(s); return n;
}
static void tick(Fixture *f)
{
    f->now += 3*SECOND; f->row.measured_at_utc_100ns = f->now;
    f->batch.started_at_utc_100ns = f->batch.finished_at_utc_100ns = f->now;
    monitor_snapshot_free(f->snapshot); f->snapshot = NULL;
    CHECK(monitor_ingest(f->engine, &f->batch, &f->path, &f->snapshot));
}
static void init(Fixture *f)
{
    char temp[MAX_PATH], error[256];
    memset(f, 0, sizeof(*f)); GetTempPathA(MAX_PATH, temp);
    snprintf(f->file, sizeof(f->file), "%srecent-fixed-%lu-%u.sqlite", temp,
        (unsigned long)GetCurrentProcessId(), ++serial);
    CHECK(GetFileAttributesA(f->file) == INVALID_FILE_ATTRIBUTES);
    CHECK(monitor_open(f->file, 1, &f->engine, error, sizeof(error)));
    if (!f->engine) { printf("open: %s\n", error); exit(2); }
    f->now = monitor_utc_now(); f->path = "SYNTHETIC:\\recent\\same.exe";
    f->row.pid = 4242; f->row.creation_time_utc_100ns = f->now - 86400*SECOND;
    f->row.process_name_utf8 = "same.exe";
    f->row.valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_CPU |
        RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
    f->row.observation_interval_seconds = f->row.cpu_interval_seconds = f->row.io_interval_seconds = 3;
    f->batch.items = &f->row; f->batch.count = 1; f->batch.logical_processor_count = 8;
    tick(f);
}
static void complete(Fixture *f)
{
    unsigned i;
    for (i = 0; i < 20; ++i) tick(f);
    CHECK(f->snapshot->completed_windows == 1);
}
static void close_fixture(Fixture *f)
{
    monitor_snapshot_free(f->snapshot); monitor_close(f->engine);
    CHECK(DeleteFileA(f->file));
}
static int trace(unsigned event, void *context, void *statement, void *sql)
{
    (void)event; (void)context; (void)statement;
    if (strstr((const char *)sql, "FROM aggregate_window w JOIN aggregate_metric m") ||
        strstr((const char *)sql, "FROM aggregate_window x INDEXED BY aggregate_program_time")) ++selects;
    return 0;
}
static MonitorRecentComparison read_recent(Fixture *f)
{
    MonitorRecentComparison out;
    CHECK(monitor_recent_read(f->engine, &f->snapshot->rows[0], &out)); return out;
}
static int64_t seed(Fixture *f, int64_t session, int64_t program, uint32_t pid,
    uint64_t creation, uint64_t start, uint64_t end, double value, unsigned mask, int state, int64_t same_run)
{
    int64_t run = same_run, id;
    unsigned m;
    if (!run) {
        execute(f, "INSERT INTO collection_run(session_id,started,finished,status,row_count,cpu_count) VALUES(%lld,%llu,%llu,0,1,8)",
            (long long)session, (unsigned long long)end, (unsigned long long)end);
        run = sqlite3_last_insert_rowid(monitor_fixture_db(f->engine));
    }
    execute(f, "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(%lld,%lld,%u,%llu,%llu,%llu,60,1)",
        (long long)run, (long long)program, pid, (unsigned long long)creation, (unsigned long long)start, (unsigned long long)end);
    id = sqlite3_last_insert_rowid(monitor_fixture_db(f->engine));
    for (m = 0; m < 4; ++m) {
        if (mask & (1u << m)) execute(f,
            "INSERT INTO aggregate_metric VALUES(%lld,%u,%.17g,60,20,1,%d,'fixed',0,0,0,NULL,NULL,NULL,0)", (long long)id, m, value, state);
        else execute(f, "INSERT INTO aggregate_metric VALUES(%lld,%u,NULL,0,0,0,0,'missing',0,0,0,NULL,NULL,NULL,0)", (long long)id, m);
    }
    return id;
}
static MonitorRecentRequest request(Fixture *f, int64_t anchor)
{
    MonitorRecentRequest q = {0};
    q.session_id = monitor_source_session(f->engine); q.program_id = f->snapshot->rows[0].program_id;
    q.anchor_window_id = anchor; q.pid = f->row.pid; q.creation = f->row.creation_time_utc_100ns;
    q.as_of_utc_100ns = f->now + 86400*SECOND; q.path = f->path; return q;
}
static MonitorRecentMetric store_read(Fixture *f, MonitorRecentRequest q)
{
    MonitorRecentMetric out;
    CHECK(monitor_store_recent_metric(monitor_fixture_store(f->engine), &q, &out)); return out;
}
/* 총 완료창 5개는 최신 1개+과거 4개여서 준비 중이다. 6번째 완료 뒤 과거 5개로
 * 준비되며, 다음 미완료 표본의 결측·일시적 부재가 저장된 최신 완료창을 바꾸지는 않는다. */
static void test_engine(void)
{
    Fixture f;
    MonitorRecentComparison out;
    int64_t previous;
    unsigned i;
    init(&f);
    sqlite3_trace_v2(monitor_fixture_db(f.engine), SQLITE_TRACE_STMT, trace, NULL);
    out = read_recent(&f); CHECK(out.metric[0].state == MONITOR_RECENT_NO_WINDOW);
    selects = 0; out = read_recent(&f); CHECK(selects == 0);
    for (i = 0; i < 5; ++i) complete(&f);
    out = read_recent(&f);
    for (i = 0; i < 4; ++i) CHECK(out.metric[i].state == MONITOR_RECENT_PREPARING && out.metric[i].count == 4);
    complete(&f); out = read_recent(&f);
    for (i = 0; i < 4; ++i) CHECK(out.metric[i].state == MONITOR_RECENT_READY && out.metric[i].count == 5 &&
        out.metric[i].current == 0 && out.metric[i].median == 0);
    previous = out.metric[0].anchor_window_id;
    selects = 0; out = read_recent(&f); CHECK(selects == 0);
    tick(&f); out = read_recent(&f); CHECK(selects == 0 && out.metric[0].anchor_window_id == previous);
    f.row.valid_fields &= ~RESOURCE_VALID_IO_READ;
    tick(&f); out = read_recent(&f); CHECK(out.metric[2].state == MONITOR_RECENT_READY);
    CHECK(f.snapshot->rows[0].metric[2].state == MONITOR_UNAVAILABLE);
    f.batch.count = 0; tick(&f); f.batch.count = 1; tick(&f);
    out = read_recent(&f); CHECK(out.metric[0].anchor_window_id == previous && out.metric[0].count == 5);
    monitor_reset(f.engine); tick(&f); out = read_recent(&f);
    CHECK(out.metric[0].state == MONITOR_RECENT_NO_WINDOW);
    for (i = 0; i < 6; ++i) complete(&f);
    out = read_recent(&f); CHECK(out.metric[0].count == 5 && out.metric[0].state == MONITOR_RECENT_READY);
    CHECK(out.metric[2].state == MONITOR_RECENT_NO_WINDOW && out.metric[2].count == 0);
    monitor_snapshot_free(f.snapshot); f.snapshot = NULL; monitor_close(f.engine); f.engine = NULL;
    { char error[256]; CHECK(monitor_open(f.file, 1, &f.engine, error, sizeof(error))); }
    tick(&f); out = read_recent(&f); CHECK(out.metric[0].state == MONITOR_RECENT_NO_WINDOW);
    for (i = 0; i < 6; ++i) complete(&f);
    out = read_recent(&f); CHECK(out.metric[0].count == 5);
    close_fixture(&f);
}
/* 잘못된 내부 요청도 직전 저장/조회 오류를 빌리지 않아야 한다. 출력 전체를 먼저
 * 오염시켜 실패 뒤 부분 결과나 이전 값이 남지 않는 계약까지 검사한다. */
static void test_engine_request_errors(void)
{
    Fixture f;
    MonitorRecentComparison out, empty = {0};
    MonitorRow changed;
    char invalid_error[512];
    init(&f);
    memset(&out, 0xa5, sizeof(out));
    CHECK(!monitor_recent_read(f.engine, NULL, &out));
    CHECK(monitor_error(f.engine)[0] && !memcmp(&out, &empty, sizeof(out)));
    snprintf(invalid_error, sizeof(invalid_error), "%s", monitor_error(f.engine));
    changed = f.snapshot->rows[0]; ++changed.raw.creation_time_utc_100ns;
    memset(&out, 0xa5, sizeof(out));
    CHECK(!monitor_recent_read(f.engine, &changed, &out));
    CHECK(monitor_error(f.engine)[0] && strcmp(monitor_error(f.engine), invalid_error));
    CHECK(!memcmp(&out, &empty, sizeof(out)));
    CHECK(monitor_recent_read(f.engine, &f.snapshot->rows[0], &out));
    CHECK(!monitor_error(f.engine)[0] && out.metric[0].state == MONITOR_RECENT_NO_WINDOW);
    close_fixture(&f);
}
static void test_boundaries(void)
{
    Fixture f;
    MonitorRecentRequest q;
    MonitorRecentMetric out;
    uint64_t start;
    int64_t anchor, program, session, run;
    unsigned i;
    init(&f); start = ((f.now + 2*3600*SECOND) / MINUTE) * MINUTE;
    program = f.snapshot->rows[0].program_id; session = monitor_source_session(f.engine);
    for (i = 0; i < 5; ++i) {
        unsigned minutes[] = {60, 59, 2, 1, 0};
        uint64_t end = start - minutes[i]*MINUTE;
        seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, end-MINUTE, end, i+1, 15, MONITOR_LEARNING, 0);
    }
    seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start-61*MINUTE-1, start-60*MINUTE-1, 999, 15, MONITOR_LEARNING, 0);
    seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start-MINUTE+1, start+1, 999, 15, MONITOR_LEARNING, 0);
    seed(&f, session, program, f.row.pid+1, f.row.creation_time_utc_100ns, start-MINUTE, start, 999, 15, MONITOR_LEARNING, 0);
    seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns+1, start-MINUTE, start, 999, 15, MONITOR_LEARNING, 0);
    execute(&f, "INSERT INTO session(started) VALUES(1)");
    seed(&f, session+1, program, f.row.pid, f.row.creation_time_utc_100ns, start-MINUTE, start, 999, 15, MONITOR_LEARNING, 0);
    execute(&f, "INSERT INTO program(path,name,first_seen,last_seen) VALUES('SYNTHETIC:\\other\\same.exe','same.exe',1,1)");
    seed(&f, session, program+1, f.row.pid, f.row.creation_time_utc_100ns, start-MINUTE, start, 999, 15, MONITOR_LEARNING, 0);
    anchor = seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start, start+MINUTE, 1000, 15, MONITOR_LEARNING, 0);
    q = request(&f, anchor); q.as_of_utc_100ns = start+MINUTE;
    out = store_read(&f, q);
    CHECK(out.state == MONITOR_RECENT_READY && out.count == 5 && out.median == 3 && out.current == 1000);
    CHECK(out.history_end_utc_100ns == start && out.history_start_utc_100ns == start-61*MINUTE);
    run = scalar(&f, "SELECT max(run_id) FROM aggregate_window");
    seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start-MINUTE, start-10*SECOND, 999, 15, MONITOR_LEARNING, run);
    out = store_read(&f, q); CHECK(out.count == 5 && out.median == 3);
    q.anchor_window_id = seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start+1, start+MINUTE+1, 10, 15, MONITOR_LEARNING, 0);
    q.as_of_utc_100ns = start+MINUTE+1;
    out = store_read(&f, q); CHECK(out.count == 4 && out.state == MONITOR_RECENT_PREPARING);
    q.path = "SYNTHETIC:\\other\\same.exe"; out = store_read(&f, q); CHECK(out.state == MONITOR_RECENT_NO_WINDOW);
    q.path = f.path; q.creation++; out = store_read(&f, q); CHECK(out.state == MONITOR_RECENT_NO_WINDOW);
    close_fixture(&f);
}
static void test_cap_quality_and_long(void)
{
    Fixture f;
    MonitorRecentRequest q;
    MonitorRecentMetric out;
    uint64_t start;
    int64_t program, session;
    unsigned i;
    init(&f); start = ((f.now+2*3600*SECOND)/MINUTE)*MINUTE;
    program = f.snapshot->rows[0].program_id; session = monitor_source_session(f.engine);
    for (i = 0; i < 36; ++i) {
        uint64_t end = start-(35-i)*MINUTE;
        seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, end-MINUTE, end, i+1, 11, MONITOR_LEARNING, 0);
    }
    seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start-MINUTE, start-SECOND, 999, 11, MONITOR_LEARNING, 0);
    q = request(&f, seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start, start+MINUTE, 0, 11, MONITOR_LEARNING, 0));
    q.as_of_utc_100ns = start+MINUTE;
    out = store_read(&f, q); CHECK(out.count == 30 && out.median == 21.5 && out.current == 0);
    CHECK(out.history_start_utc_100ns == start-30*MINUTE);
    q.metric = MONITOR_READ; out = store_read(&f, q); CHECK(out.state == MONITOR_RECENT_NO_WINDOW && out.count == 0);
    q.metric = MONITOR_WRITE; out = store_read(&f, q); CHECK(out.count == 30 && out.median == 21.5);
    q.anchor_window_id = seed(&f, session, program, f.row.pid, f.row.creation_time_utc_100ns, start+MINUTE, start+2*MINUTE, 10, 15, MONITOR_NORMAL, 0);
    q.as_of_utc_100ns = start+2*MINUTE;
    out = store_read(&f, q); CHECK(out.state == MONITOR_RECENT_LONG_TERM && out.count == 0);
    close_fixture(&f);
}
/* bridge가 가진 최신 view의 세션·회차·PID+생성시각·program/path가 모두 맞아야
 * 조회를 허용한다. pause는 그 view를 보존하고 resume 후 새 수집은 창 연결을 새로 시작한다. */
static void test_bridge(void)
{
    Bridge *b;
    char error[256], temp[MAX_PATH], file[MAX_PATH];
    const BridgeView *v = NULL;
    MonitorRecentComparison out;
    unsigned i, session;
    uint64_t seq;
    GetTempPathA(MAX_PATH, temp);
    snprintf(file, sizeof(file), "%srecent-bridge-%lu.sqlite", temp, (unsigned long)GetCurrentProcessId());
    b = bridge_open(file, 1, error, sizeof(error)); CHECK(b != NULL);
    if (!b) { printf("bridge: %s\n", error); return; }
    for (i = 0; i < 121; ++i) v = bridge_collect(b);
    CHECK(v && v->snapshot && v->snapshot->count == 1);
    session = v->session; seq = v->snapshot->sequence;
    const MonitorRow *r = &v->snapshot->rows[0];
    CHECK(bridge_recent(b,session,seq,r->raw.pid,r->raw.creation_time_utc_100ns,r->program_id,r->path,&out));
    CHECK(out.metric[0].state == MONITOR_RECENT_READY && out.metric[0].count == 5);
    CHECK(!bridge_recent(b,session,seq-1,r->raw.pid,r->raw.creation_time_utc_100ns,r->program_id,r->path,&out));
    CHECK(!bridge_recent(b,session+1,seq,r->raw.pid,r->raw.creation_time_utc_100ns,r->program_id,r->path,&out));
    CHECK(!bridge_recent(b,session,seq,r->raw.pid,r->raw.creation_time_utc_100ns+1,r->program_id,r->path,&out));
    CHECK(!bridge_recent(b,session,seq,r->raw.pid,r->raw.creation_time_utc_100ns,r->program_id,"wrong-path",&out));
    CHECK(bridge_sizeof(11) == 72 && bridge_sizeof(12) == 288);
    CHECK(bridge_pause(b));
    CHECK(bridge_recent(b,session,seq,r->raw.pid,r->raw.creation_time_utc_100ns,r->program_id,r->path,&out));
    CHECK(out.metric[0].state == MONITOR_RECENT_READY);
    CHECK(bridge_resume(b)); v = bridge_collect(b);
    r = &v->snapshot->rows[0];
    CHECK(bridge_recent(b,v->session,v->snapshot->sequence,r->raw.pid,r->raw.creation_time_utc_100ns,r->program_id,r->path,&out));
    CHECK(out.metric[0].state == MONITOR_RECENT_NO_WINDOW);
    bridge_close(b); CHECK(DeleteFileA(file));
}
static void test_utc_reversal(void)
{
    unsigned cached, i;
    for (cached = 0; cached < 2; ++cached) {
        Fixture f;
        MonitorRecentComparison out;
        uint64_t latest;
        init(&f);
        for (i = 0; i < 6; ++i) complete(&f);
        latest = f.now;
        if (cached) { out = read_recent(&f); CHECK(out.metric[0].window_end_utc_100ns == latest); }
        f.now -= 90*SECOND; tick(&f); out = read_recent(&f);
        CHECK(out.metric[0].window_end_utc_100ns <= f.now);
        CHECK(out.metric[0].window_end_utc_100ns < latest);
        f.batch.count = 0; tick(&f); f.batch.count = 1; tick(&f);
        out = read_recent(&f); CHECK(out.metric[0].window_end_utc_100ns <= f.now);
        f.now += 90*SECOND; tick(&f); out = read_recent(&f);
        CHECK(out.metric[0].window_end_utc_100ns == latest);
        CHECK(out.metric[0].state == MONITOR_RECENT_READY);
        close_fixture(&f);
    }
}
static void test_engine_long_transition(void)
{
    Fixture f;
    MonitorRecentComparison out;
    unsigned day, i;
    int64_t program, session;
    init(&f);
    for (i = 0; i < 6; ++i) complete(&f);
    out = read_recent(&f); CHECK(out.metric[0].state == MONITOR_RECENT_READY);
    program = f.snapshot->rows[0].program_id; session = monitor_source_session(f.engine);
    for (day = 1; day <= 3; ++day) for (i = 0; i < 20; ++i) {
        uint64_t end = f.now - day*86400*SECOND - i*MINUTE;
        seed(&f, session, program, 9999, 1, end-MINUTE, end, 0, 15, MONITOR_LEARNING, 0);
    }
    complete(&f); out = read_recent(&f);
    for (i = 0; i < 4; ++i) CHECK(f.snapshot->rows[0].metric[i].state == MONITOR_NORMAL &&
        out.metric[i].state == MONITOR_RECENT_LONG_TERM);
    f.row.valid_fields &= ~RESOURCE_VALID_CPU; tick(&f); out = read_recent(&f);
    CHECK(f.snapshot->rows[0].metric[0].state == MONITOR_UNAVAILABLE);
    CHECK(out.metric[0].state == MONITOR_RECENT_LONG_TERM);
    close_fixture(&f);
}
static void test_backdated_completion_reappearance(void)
{
    Fixture f;
    MonitorRecentComparison old, continuing, reappeared;
    unsigned i;
    init(&f);
    for (i = 0; i < 6; ++i) complete(&f);
    old = read_recent(&f);
    f.now -= 120*SECOND; tick(&f); complete(&f);
    continuing = read_recent(&f);
    CHECK(continuing.metric[0].anchor_window_id > old.metric[0].anchor_window_id);
    CHECK(continuing.metric[0].window_end_utc_100ns < old.metric[0].window_end_utc_100ns);
    f.row.valid_fields &= ~RESOURCE_VALID_CPU;
    f.now += 60*SECOND; tick(&f); continuing = read_recent(&f);
    f.batch.count = 0; tick(&f); f.batch.count = 1; tick(&f);
    reappeared = read_recent(&f);
    CHECK(reappeared.metric[0].anchor_window_id == continuing.metric[0].anchor_window_id);
    CHECK(reappeared.metric[0].window_end_utc_100ns == continuing.metric[0].window_end_utc_100ns);
    /* 최초 탐색 뒤 반복 읽기는 COMMIT된 창 정보·SQL 대체 조회와 같은 창을 가리켜야 하며
     * 이미 찾은 창을 다시 탐색하지 않는다. */
    sqlite3_trace_v2(monitor_fixture_db(f.engine), SQLITE_TRACE_STMT, trace, NULL);
    selects = 0; reappeared = read_recent(&f); CHECK(selects == 0);
    close_fixture(&f);
}
/* 보관 정리는 최신 창뿐 아니라 그 이전 비교 범위도 바꾼다. 동일 합성 입력의 최초 조회와
 * 미리 채운 캐시를 대조하며, ingest가 공개할 next와 feedback의 현재 상태를 함께 검사한다.
 * collection_run 삭제 실패는 aggregate 삭제 뒤에 발생시켜 정리 전체 rollback을 검증한다. */
static void retention_gap(Fixture *f, uint64_t at)
{
    f->row.valid_fields &= ~(RESOURCE_VALID_OBSERVATION_INTERVAL | RESOURCE_VALID_CPU |
        RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE);
    f->row.observation_interval_seconds = f->row.cpu_interval_seconds = f->row.io_interval_seconds = NAN;
    f->row.cpu_percent = f->row.io_read_bytes_per_second = f->row.io_write_bytes_per_second = NAN;
    f->row.cpu_status = f->row.io_read_status = f->row.io_write_status = RESOURCE_DELTA_LONG_GAP;
    f->now = at - 3*SECOND;
    tick(f);
}
static void retention_write(Fixture *f, unsigned feedback, uint64_t at)
{
    if (feedback) CHECK(monitor_feedback(f->engine, f->snapshot->rows[0].program_id,
        MONITOR_FEEDBACK_IGNORE, at));
    else retention_gap(f, at);
}
static void compare_recent(const MonitorRecentComparison *a, const MonitorRecentComparison *b)
{
    unsigned m;
    for (m = 0; m < MONITOR_METRICS; ++m) {
        const MonitorRecentMetric *x = &a->metric[m], *y = &b->metric[m];
        CHECK(x->state == y->state && x->count == y->count && x->anchor_window_id == y->anchor_window_id);
        CHECK(x->current == y->current && x->median == y->median &&
            x->window_duration_seconds == y->window_duration_seconds);
        CHECK(x->window_start_utc_100ns == y->window_start_utc_100ns &&
            x->window_end_utc_100ns == y->window_end_utc_100ns &&
            x->history_start_utc_100ns == y->history_start_utc_100ns &&
            x->history_end_utc_100ns == y->history_end_utc_100ns);
    }
}
static void test_retention_cache(void)
{
    unsigned scenario, feedback, rollback, cached;
    for (scenario = 0; scenario < 3; ++scenario)
    for (feedback = 0; feedback < 2; ++feedback)
    for (rollback = 0; rollback < 2; ++rollback) {
        MonitorRecentComparison control = {0};
        for (cached = 0; cached < 2; ++cached) {
            Fixture f;
            MonitorRecentComparison out;
            unsigned i, m, windows = scenario == 0 ? 6 : 1;
            uint64_t latest, trim_at;
            init(&f);
            /* 시작 시각과 생성시각까지 고정해 두 DB의 실제 입력을 같게 한다. */
            monitor_reset(f.engine);
            f.now = UINT64_C(134390000000000000);
            f.row.creation_time_utc_100ns = f.now - 86400*SECOND;
            f.row.cpu_percent = 10; f.row.working_set_bytes = 70000000;
            tick(&f);
            for (i = 0; i < windows; ++i) complete(&f);
            latest = f.now;
            trim_at = latest + (scenario == 0 ? 28*86400*SECOND - 30*SECOND : 29*86400*SECOND);
            sqlite3_trace_v2(monitor_fixture_db(f.engine), SQLITE_TRACE_STMT, trace, NULL);
            if (cached) {
                out = read_recent(&f);
                CHECK(out.metric[0].current == 10 && out.metric[0].window_end_utc_100ns == latest);
                CHECK(out.metric[0].count == windows - 1);
            }
            if (rollback) execute(&f, "CREATE TRIGGER stop_recent_trim BEFORE DELETE ON collection_run "
                "BEGIN SELECT RAISE(ABORT,'recent trim rollback after aggregate delete'); END");
            retention_write(&f, feedback, trim_at);
            if (rollback) {
                CHECK(monitor_retention_pending(f.engine));
                CHECK(scalar(&f, "SELECT count(*) FROM aggregate_window") == (int64_t)windows);
                CHECK(scalar(&f, "SELECT count(*) FROM feedback") == (feedback ? 1 : 0));
                selects = 0; out = read_recent(&f);
                CHECK(out.metric[0].current == 10 && out.metric[0].count == windows - 1 &&
                    out.metric[0].window_end_utc_100ns == latest);
                if (cached) CHECK(selects == 0); /* rollback은 유효한 캐시를 버리지 않는다. */
                execute(&f, "DROP TRIGGER stop_recent_trim");
                retention_write(&f, feedback, trim_at + 3*SECOND);
            }
            CHECK(!monitor_retention_pending(f.engine));
            CHECK(scalar(&f, "SELECT count(*) FROM aggregate_window") == (scenario == 0 ? 1 : 0));
            if (scenario == 2) {
                /* 삭제된 ID 1을 새 CPU 25% 창이 재사용하기 전에는 조회하지 않는다. */
                if (feedback) retention_gap(&f, trim_at + 6*SECOND);
                f.row.valid_fields |= RESOURCE_VALID_OBSERVATION_INTERVAL | RESOURCE_VALID_CPU |
                    RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE;
                f.row.observation_interval_seconds = f.row.cpu_interval_seconds = f.row.io_interval_seconds = 3;
                f.row.cpu_percent = 25; f.row.io_read_bytes_per_second = f.row.io_write_bytes_per_second = 0;
                f.row.cpu_status = f.row.io_read_status = f.row.io_write_status = RESOURCE_DELTA_VALID;
                complete(&f);
                CHECK(scalar(&f, "SELECT count(*) FROM aggregate_window") == 1);
            }
            out = read_recent(&f);
            for (m = 0; m < MONITOR_METRICS; ++m) {
                CHECK(out.metric[m].count == 0 && out.metric[m].median == 0 &&
                    out.metric[m].history_start_utc_100ns == 0 && out.metric[m].history_end_utc_100ns == 0);
                CHECK(out.metric[m].state == (unsigned)(scenario == 1 ? MONITOR_RECENT_NO_WINDOW : MONITOR_RECENT_PREPARING));
            }
            if (scenario == 1) CHECK(out.metric[0].anchor_window_id == 0 && out.metric[0].current == 0 &&
                out.metric[0].window_start_utc_100ns == 0 && out.metric[0].window_end_utc_100ns == 0);
            else CHECK(out.metric[0].anchor_window_id == (scenario == 0 ? 6 : 1) &&
                out.metric[0].current == (scenario == 0 ? 10 : 25) &&
                out.metric[0].window_end_utc_100ns == (scenario == 0 ? latest : f.now));
            if (cached) compare_recent(&control, &out); else control = out;
            printf("retention scenario=%u feedback=%u rollback=%u cached=%u state=%u count=%u CPU=%.1f end=%llu\n",
                scenario, feedback, rollback, cached, out.metric[0].state, out.metric[0].count,
                out.metric[0].current, (unsigned long long)out.metric[0].window_end_utc_100ns);
            selects = 0; out = read_recent(&f); CHECK(selects == 0);
            /* 삭제 없는 화면 갱신·일반 회차와 raw만 삭제하는 정리도 SQL을 늘리지 않는다. */
            if (scenario != 1) {
                tick(&f); out = read_recent(&f); CHECK(selects == 0);
            }
            close_fixture(&f);
        }
    }
}
static void test_raw_only_retention_cache(void)
{
    Fixture f;
    MonitorRecentComparison before, after;
    unsigned i;
    init(&f);
    for (i = 0; i < 6; ++i) complete(&f);
    before = read_recent(&f);
    sqlite3_trace_v2(monitor_fixture_db(f.engine), SQLITE_TRACE_STMT, trace, NULL);
    selects = 0;
    retention_gap(&f, f.now + 2*3600*SECOND);
    CHECK(scalar(&f, "SELECT count(*) FROM raw") == 1);
    CHECK(scalar(&f, "SELECT count(*) FROM aggregate_window") == 6);
    after = read_recent(&f); CHECK(selects == 0); compare_recent(&before, &after);
    CHECK(monitor_feedback(f.engine, f.snapshot->rows[0].program_id,
        MONITOR_FEEDBACK_CANCEL, f.now + 2*3600*SECOND));
    CHECK(scalar(&f, "SELECT count(*) FROM raw") == 0);
    after = read_recent(&f); CHECK(selects == 0); compare_recent(&before, &after);
    close_fixture(&f);
}
int main(void)
{
    test_engine(); test_engine_request_errors(); test_boundaries(); test_cap_quality_and_long(); test_bridge();
    test_utc_reversal(); test_engine_long_transition(); test_backdated_completion_reappearance();
    test_retention_cache(); test_raw_only_retention_cache();
    printf("recent comparison: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
