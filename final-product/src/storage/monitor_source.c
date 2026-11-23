/* COMMIT 결과를 snapshot에 연결하고 prototype source의 짝 확인·진단을 제공한다.
 * engine 내부 상태는 읽지 않으며 전달된 참조와 실제 저장된 회차·창을 대조한다.
 * 선택 상세용 최근 비교는 monitor_store_recent_metric에서 anchor→과거 이력 순으로 읽는다.
 * 각 조회는 자기 statement를 해제하며, 성공한 값 사본만 호출자에게 공개한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "monitor_store_private.h"
#include "monitor_source_store.h"
#include "monitor_recent_store.h"

static int recent_double_compare(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return (a > b) - (a < b);
}
/* 같은 세션·경로·인스턴스·지표의 완료창을 선택한다. 정확한 ID를 요청하면 그 창을 검증하고,
 * ID가 0이면 저장 성공 순서가 가장 늦은 창을 찾는다. 호출자가 0 초기화한 result를 받는다.
 * 일치하는 창 없음은 1을 반환하고 그 빈 결과를 유지한다. SQL 실패만 0을 반환한다. */
static int read_recent_anchor(MonitorStore *store, const MonitorRecentRequest *q,
    MonitorRecentMetric *result, int64_t *anchor_run)
{
    static const char anchor_sql[] =
        "SELECT w.id,w.run_id,w.start_utc,w.end_utc,w.duration,m.value,m.state "
        "FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id AND m.metric=?1 "
        "JOIN collection_run r ON r.id=w.run_id JOIN program p ON p.id=w.program_id "
        "WHERE w.program_id=?2 AND p.path=?3 COLLATE BINARY AND w.pid=?4 AND w.creation=?5 "
        "AND r.session_id=?6 AND r.status=0 AND w.run_id>=?7 AND w.version=" MONITOR_POLICY_TEXT(MONITOR_AGGREGATION_VERSION) " "
        "AND w.end_utc>=?5 AND w.end_utc<=?8 "
        "AND m.state BETWEEN 1 AND 4 AND m.value IS NOT NULL AND m.value>=0 "
        "AND m.value<=1.7976931348623157e308 ";
    sqlite3_stmt *s = NULL;
    int rc, state;
    char sql[2048];
    /* UTC가 역행해도 QPC로 새 창이 완성될 수 있으므로 최신 여부는 run_id,id로 정한다.
     * 특정 anchor는 INTEGER PRIMARY KEY, 최초 탐색은 기존 program/end 인덱스와 정렬을 쓴다. */
    snprintf(sql, sizeof(sql), "%s%s ORDER BY w.run_id DESC,w.id DESC LIMIT 1", anchor_sql,
        q->anchor_window_id ? "AND w.id=?9" : "AND ?9=0");
    if (sqlite3_prepare_v2(store->db, sql, -1, &s, NULL) != SQLITE_OK) goto failed;
    sqlite3_bind_int(s, 1, (int)q->metric); sqlite3_bind_int64(s, 2, q->program_id);
    sqlite3_bind_text(s, 3, q->path, -1, SQLITE_TRANSIENT); sqlite3_bind_int64(s, 4, q->pid);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)q->creation); sqlite3_bind_int64(s, 6, q->session_id);
    sqlite3_bind_int64(s, 7, q->first_run_id); sqlite3_bind_int64(s, 8, (sqlite3_int64)q->as_of_utc_100ns);
    sqlite3_bind_int64(s, 9, q->anchor_window_id);
    rc = sqlite3_step(s);
    if (rc == SQLITE_DONE) { sqlite3_finalize(s); return 1; }
    if (rc != SQLITE_ROW) goto failed;
    result->anchor_window_id = sqlite3_column_int64(s, 0); *anchor_run = sqlite3_column_int64(s, 1);
    result->window_start_utc_100ns = (uint64_t)sqlite3_column_int64(s, 2);
    result->window_end_utc_100ns = (uint64_t)sqlite3_column_int64(s, 3);
    result->window_duration_seconds = sqlite3_column_double(s, 4); result->current = sqlite3_column_double(s, 5);
    state = sqlite3_column_int(s, 6);
    sqlite3_finalize(s);
    result->state = state == MONITOR_LEARNING ? MONITOR_RECENT_PREPARING : MONITOR_RECENT_LONG_TERM;
    return 1;
failed:
    monitor_store_set_error(store, "recent display query");
    sqlite3_finalize(s);
    return 0;
}

/* anchor와 별도인 이전 저장 회차에서 정책 범위의 유효 값을 읽는다. SQL의 end_utc,id 내림차순을
 * 이용해 같은 종료 분의 첫 행만 채택하며 정책 상한까지만 모은다. 0은 유효값, SQL NULL은 결측이다.
 * 결과는 호출자의 임시 구조체에만 누적되므로 중간 SQL 실패가 화면용 부분 결과로 노출되지 않는다. */
static int read_recent_history(MonitorStore *store, const MonitorRecentRequest *q,
    int64_t anchor_run, MonitorRecentMetric *result)
{
    static const char history_sql[] =
        "SELECT x.start_utc,x.end_utc,m.value "
        "FROM aggregate_window x INDEXED BY aggregate_program_time "
        "JOIN aggregate_metric m ON m.window_id=x.id AND m.metric=?1 "
        "JOIN collection_run r ON r.id=x.run_id "
        "WHERE x.program_id=?2 AND x.pid=?3 AND x.creation=?4 AND x.version=" MONITOR_POLICY_TEXT(MONITOR_AGGREGATION_VERSION) " "
        "AND r.session_id=?5 AND r.status=0 AND x.run_id>=?6 AND x.run_id<?9 AND x.id<?10 "
        "AND x.end_utc>=?7 AND x.end_utc<=?8 "
        "AND m.state BETWEEN 1 AND 4 AND m.value IS NOT NULL AND m.value>=0 "
        "AND m.value<=1.7976931348623157e308 "
        "ORDER BY x.end_utc DESC,x.id DESC";
    sqlite3_stmt *s = NULL;
    double values[MONITOR_RECENT_MAX_HISTORY];
    int rc;
    uint64_t previous_minute = UINT64_MAX;
    uint64_t lower = result->window_start_utc_100ns > MONITOR_RECENT_HISTORY_TICKS ?
        result->window_start_utc_100ns - MONITOR_RECENT_HISTORY_TICKS : 0;
    if (sqlite3_prepare_v2(store->db, history_sql, -1, &s, NULL) != SQLITE_OK) goto failed;
    sqlite3_bind_int(s, 1, (int)q->metric); sqlite3_bind_int64(s, 2, q->program_id);
    sqlite3_bind_int64(s, 3, q->pid); sqlite3_bind_int64(s, 4, (sqlite3_int64)q->creation);
    sqlite3_bind_int64(s, 5, q->session_id); sqlite3_bind_int64(s, 6, q->first_run_id);
    sqlite3_bind_int64(s, 7, (sqlite3_int64)lower);
    sqlite3_bind_int64(s, 8, (sqlite3_int64)result->window_start_utc_100ns);
    sqlite3_bind_int64(s, 9, anchor_run); sqlite3_bind_int64(s, 10, result->anchor_window_id);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        uint64_t start = (uint64_t)sqlite3_column_int64(s, 0), end = (uint64_t)sqlite3_column_int64(s, 1);
        double value = sqlite3_column_double(s, 2);
        uint64_t minute = end / MONITOR_MINUTE_TICKS;
        if (minute == previous_minute) continue;
        if (!isfinite(value) || value < 0) goto failed;
        previous_minute = minute;
        values[result->count++] = value;
        if (!result->history_start_utc_100ns || start < result->history_start_utc_100ns) result->history_start_utc_100ns = start;
        if (end > result->history_end_utc_100ns) result->history_end_utc_100ns = end;
        if (result->count == MONITOR_RECENT_MAX_HISTORY) break;
    }
    if (rc != SQLITE_DONE && !(rc == SQLITE_ROW && result->count == MONITOR_RECENT_MAX_HISTORY)) goto failed;
    sqlite3_finalize(s);
    if (result->count) {
        unsigned middle = result->count / 2;
        qsort(values, result->count, sizeof(*values), recent_double_compare);
        result->median = result->count % 2 ? values[middle] : values[middle - 1] / 2 + values[middle] / 2;
    }
    if (result->count >= MONITOR_RECENT_MIN_HISTORY) result->state = MONITOR_RECENT_READY;
    return 1;
failed:
    monitor_store_set_error(store, "recent display query");
    sqlite3_finalize(s);
    return 0;
}

/* 출력 포인터가 있으면 입력 검증 전에 비운다. 유효한 요청의 조회 성공은 1이며, 일치하는 창이
 * 없어도 성공이다. 이때 state=MONITOR_RECENT_NO_WINDOW, anchor_window_id=0인 빈 결과를 반환한다.
 * 잘못된 인자 또는 SQL 실패는 0을 반환하고 출력은 빈 상태로 남는다. 따라서 빈 결과의 수치 0만
 * 보고 조회 성공이나 유효한 0 측정으로 해석하지 않는다. 먼저 반환값, 이어 state/anchor를 확인한다.
 * 필요한 조회를 모두 마친 값 사본만 공개하며 statement나 동적 메모리 소유권을 넘기지 않는다. */
int monitor_store_recent_metric(MonitorStore *store, const MonitorRecentRequest *q, MonitorRecentMetric *out)
{
    MonitorRecentMetric result = {0};
    int64_t anchor_run = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!store || !q || !out || !q->path || !q->path[0] || q->metric >= MONITOR_METRICS ||
        q->program_id <= 0 || q->session_id <= 0 || !q->creation || q->creation > INT64_MAX ||
        q->as_of_utc_100ns > INT64_MAX) return 0;
    if (!read_recent_anchor(store, q, &result, &anchor_run)) return 0;
    /* 완료창이 없거나 장기 기준이 준비된 지표에는 최근 중앙값을 만들지 않는다. */
    if (result.anchor_window_id && result.state != MONITOR_RECENT_LONG_TERM &&
        !read_recent_history(store, q, anchor_run, &result)) return 0;
    *out = result;
    return 1;
}

static void source_error(MonitorStore *store, const char *message)
{ snprintf(store->error, sizeof(store->error), "%s", message); }
static const char *source_text(sqlite3_stmt *statement, int column)
{
    const unsigned char *p = sqlite3_column_text(statement, column);
    return p ? (const char *)p : "";
}
void monitor_source_windows_free(MonitorCommittedWindows *windows)
{
    if (!windows) return;
    free(windows->items);
    memset(windows, 0, sizeof(*windows));
}
static int window_metadata_matches(sqlite3_stmt *statement, const MonitorRow *row)
{
    unsigned metric;
    if (row->program_id != sqlite3_column_int64(statement, 4)) return 0;
    for (metric = 0; metric < MONITOR_METRICS; ++metric) {
        const MonitorMetricReport *report = &row->metric[metric];
        if (report->window_start_utc_100ns != (uint64_t)sqlite3_column_int64(statement, 5) ||
            report->window_end_utc_100ns != (uint64_t)sqlite3_column_int64(statement, 6) ||
            report->window_duration_seconds != sqlite3_column_double(statement, 7)) return 0;
    }
    return 1;
}
/* 검증된 COMMIT 참조에서 실제 저장 회차·완료창을 다시 읽어 snapshot 행과 연결한다.
 * 반환 배열만 새 소유권이며 row는 snapshot을 빌린다. 창 수·메타데이터 불일치나
 * SQL 실패에는 임시 배열을 버린다. 중복 신원의 첫 행 선택은 match_flags로 드러낸다. */
int monitor_store_source_read(MonitorStore *store, const MonitorCommitRef *ref,
    const MonitorSnapshot *snapshot, MonitorCommittedWindows *out)
{
    sqlite3_stmt *statement = NULL;
    MonitorCommittedWindows pending = {0};
    int rc;
    /* 완료창이 0개여도 원래 COMMIT된 collection_run이 존재해야 한다. */
    if (sqlite3_prepare_v2(store->db,
        "SELECT finished,status,row_count,foreground_utc,foreground_pid FROM collection_run WHERE id=? AND session_id=?",
        -1, &statement, NULL) != SQLITE_OK) goto sql_failed;
    sqlite3_bind_int64(statement, 1, ref->run_id);
    sqlite3_bind_int64(statement, 2, ref->session_id);
    rc = sqlite3_step(statement);
    if (rc != SQLITE_ROW) {
        if (rc != SQLITE_DONE) goto sql_failed;
        source_error(store, "Committed source collection run is unavailable");
        goto failed;
    }
    if ((uint64_t)sqlite3_column_int64(statement, 0) != snapshot->measured_utc_100ns ||
        sqlite3_column_int(statement, 1) != 0 ||
        (uint64_t)sqlite3_column_int64(statement, 2) != snapshot->count ||
        (uint64_t)sqlite3_column_int64(statement, 3) != snapshot->foreground_observed_utc_100ns ||
        (uint32_t)sqlite3_column_int64(statement, 4) != snapshot->foreground_pid) {
        source_error(store, "Committed source collection run does not match snapshot");
        goto failed;
    }
    rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) goto sql_failed;
    sqlite3_finalize(statement);
    statement = NULL;
    if (snapshot->completed_windows) {
        if (snapshot->completed_windows > SIZE_MAX / sizeof(*pending.items) ||
            !(pending.items = calloc(snapshot->completed_windows, sizeof(*pending.items)))) {
            source_error(store, "Out of memory reading committed source windows");
            goto failed;
        }
    }
    if (sqlite3_prepare_v2(store->db,
        "SELECT w.id,w.run_id,w.pid,w.creation,w.program_id,w.start_utc,w.end_utc,w.duration "
        "FROM aggregate_window w JOIN collection_run r ON r.id=w.run_id "
        "WHERE w.id>? AND w.run_id=? AND r.session_id=? ORDER BY w.id",
        -1, &statement, NULL) != SQLITE_OK) goto sql_failed;
    sqlite3_bind_int64(statement, 1, ref->history_cutoff);
    sqlite3_bind_int64(statement, 2, ref->run_id);
    sqlite3_bind_int64(statement, 3, ref->session_id);
    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        uint32_t pid = (uint32_t)sqlite3_column_int64(statement, 2);
        uint64_t creation = (uint64_t)sqlite3_column_int64(statement, 3);
        size_t i, first = snapshot->count, matches = 0;
        MonitorCommittedWindow *window;
        int metadata_matches;
        for (i = 0; i < snapshot->count; ++i) {
            if (snapshot->rows[i].raw.pid == pid && snapshot->rows[i].raw.creation_time_utc_100ns == creation) {
                if (!matches) first = i;
                ++matches;
            }
        }
        if (!matches) {
            source_error(store, "Committed window has no matching immutable snapshot row");
            goto failed;
        }
        if (pending.count >= snapshot->completed_windows) {
            source_error(store, "Committed source window count does not match snapshot");
            goto failed;
        }
        metadata_matches = window_metadata_matches(statement, &snapshot->rows[first]);
        if (matches == 1 && !metadata_matches) {
            source_error(store, "Committed source window metadata does not match snapshot");
            goto failed;
        }
        /* 공개 배치는 중복 신원을 허용한다. 저장된 창의 메타데이터가 다른 행에서
         * 왔더라도 기존처럼 신원이 처음 일치하는 행을 연결한다. */
        window = &pending.items[pending.count++];
        window->window_id = sqlite3_column_int64(statement, 0);
        window->run_id = sqlite3_column_int64(statement, 1);
        window->row_index = first;
        window->row = &snapshot->rows[first];
        window->match_flags = (matches > 1 ? MONITOR_SOURCE_DUPLICATE_TUPLE : 0) |
            (!metadata_matches ? MONITOR_SOURCE_METADATA_MISMATCH : 0);
    }
    if (rc != SQLITE_DONE) goto sql_failed;
    if (pending.count != snapshot->completed_windows) {
        source_error(store, "Committed source window count does not match snapshot");
        goto failed;
    }
    sqlite3_finalize(statement);
    *out = pending;
    return 1;
sql_failed:
    source_error(store, sqlite3_errmsg(store->db));
failed:
    sqlite3_finalize(statement);
    monitor_source_windows_free(&pending);
    return 0;
}
/* 새 실험 source를 식별할 짝을 기록한다. CREATE와 INSERT는 각각 autocommit이므로
 * 뒤 INSERT가 실패하면 빈 짝 테이블이 남을 수 있다. sidecar와의 원자적 COMMIT은 아니다. */
int monitor_store_source_create_pair(MonitorStore *store, const char *pair)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_exec(store->db, "CREATE TABLE _experiment_pair(pair TEXT NOT NULL)", NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db, "INSERT INTO _experiment_pair VALUES(?)", -1, &statement, NULL) != SQLITE_OK)
        goto failed;
    sqlite3_bind_text(statement, 1, pair, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_DONE) goto failed;
    sqlite3_finalize(statement);
    return 1;
failed:
    source_error(store, sqlite3_errmsg(store->db));
    sqlite3_finalize(statement);
    return 0;
}
static int source_local_path(const char *path)
{
    wchar_t wide[32768], full[32768], root[4];
    DWORD n;
    if (!path || !path[0] || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768)) return 0;
    n = GetFullPathNameW(wide, 32768, full, NULL);
    if (!n || n >= 32768 || full[0] == L'\\' || full[1] != L':') return 0;
    root[0] = full[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    return GetDriveTypeW(root) != DRIVE_REMOTE;
}
int monitor_source_pair_matches(const char *path, const char *pair)
{
    sqlite3 *source = NULL;
    sqlite3_stmt *statement = NULL;
    int ok = 0;
    if (!pair || !source_local_path(path)) return 0;
    if (sqlite3_open_v2(path, &source, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(source, "SELECT pair FROM _experiment_pair", -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW && strcmp(source_text(statement, 0), pair) == 0 &&
        sqlite3_step(statement) == SQLITE_DONE) ok = 1;
    sqlite3_finalize(statement);
    sqlite3_close(source);
    return ok;
}

void monitor_source_diagnostics(const char *path, const char *pair, MonitorSourceDiagnostics *out)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    int64_t run = 0;
    unsigned m;
    static const unsigned metric_bits[4] = { RESOURCE_VALID_CPU, RESOURCE_VALID_MEMORY, RESOURCE_VALID_IO_READ, RESOURCE_VALID_IO_WRITE };
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->source_available = monitor_source_pair_matches(path, pair);
    /* 진단 자료가 없으면 available=0으로 남기며 결측 0인 성공으로 만들지 않는다.
       raw_run 인덱스로 한 수집 회차의 원시 행을 조회한다. */
    if (!out->source_available || sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto finished;
    /* 후속 SELECT들은 단일 읽기 트랜잭션이 아니므로 외부 동시 쓰기의 동일 시점을 보장하지 않는다. */
    sqlite3_busy_timeout(db, 250);
    if (sqlite3_prepare_v2(db, "SELECT id,finished,status,row_count FROM collection_run ORDER BY id DESC LIMIT 1", -1,
                           &s, NULL) != SQLITE_OK ||
        sqlite3_step(s) != SQLITE_ROW)
        goto finished;
    run = sqlite3_column_int64(s, 0);
    out->utc = (uint64_t)sqlite3_column_int64(s, 1);
    out->status = sqlite3_column_int(s, 2);
    out->rows = (uint64_t)sqlite3_column_int64(s, 3);
    sqlite3_finalize(s);
    s = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT count(*),coalesce(sum((valid&?)=0),0),coalesce(sum(program_id IS NULL OR path_error<>0),0),"
            "coalesce(sum((valid&?)=0 OR cpu IS NULL),0),coalesce(sum((valid&?)=0 OR ram IS NULL),0),coalesce(sum((valid&?)=0 OR io_read IS NULL),0),coalesce(sum((valid&?)=0 OR io_write IS NULL),0) FROM raw WHERE run_id=?",
            -1, &s, NULL) != SQLITE_OK)
        goto finished;
    sqlite3_bind_int64(s, 1, RESOURCE_VALID_IDENTITY);
    for (m = 0; m < MONITOR_METRICS; ++m)
        sqlite3_bind_int64(s, (int)m + 2, metric_bits[m]);
    sqlite3_bind_int64(s, 6, run);
    if (sqlite3_step(s) != SQLITE_ROW)
        goto finished;
    out->raw_rows = (uint64_t)sqlite3_column_int64(s, 0);
    out->identity_unavailable = (uint64_t)sqlite3_column_int64(s, 1);
    out->path_unavailable = (uint64_t)sqlite3_column_int64(s, 2);
    for (m = 0; m < MONITOR_METRICS; ++m)
        out->metric_missing[m] = (uint64_t)sqlite3_column_int64(s, (int)m + 3);
    out->available = out->raw_rows == out->rows;
finished:
    sqlite3_finalize(s);
    sqlite3_close(db);
}
