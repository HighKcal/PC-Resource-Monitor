/* COMMIT 결과를 snapshot에 연결하고 prototype source의 짝 확인·진단을 제공한다.
 * engine 내부 상태는 읽지 않으며 전달된 참조와 실제 저장된 회차·창을 대조한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor_store_private.h"
#include "monitor_source_store.h"

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
