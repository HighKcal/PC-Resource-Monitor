/* 실험 DB의 schema·설정 검사와 근거 저장 SQL을 담당한다. 연결·오류 버퍼는
 * 공유 context에서 빌리고 임시 statement를 정리한다. Track과 core 내부 상태는 수정하지 않는다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include "experiment_internal.h"
#include "experiment_retention.h"

/* 비교 실험용 고정 상한: 전체 논리 CPU 용량 대비 %, WorkingSet bytes, 읽기·쓰기 B/s. */
const double exp_fixed_limits[4] = {25, 536870912, 10485760, 10485760};
static const char schema[] =
    "PRAGMA application_id=1163415601; PRAGMA user_version=1;"
    "CREATE TABLE experiment(pair TEXT NOT NULL,source TEXT NOT NULL,synthetic INTEGER NOT NULL,"
    "policy TEXT NOT NULL,template TEXT NOT NULL,replay INTEGER NOT NULL,retention_days INTEGER NOT NULL,"
    "cpu_limit REAL NOT NULL,ram_limit REAL NOT NULL,read_limit REAL NOT NULL,write_limit REAL NOT NULL);"
    "CREATE TABLE operation(id INTEGER PRIMARY KEY AUTOINCREMENT,kind INTEGER NOT NULL,utc INTEGER NOT NULL,"
    "session INTEGER NOT NULL,pid INTEGER NOT NULL,creation INTEGER NOT NULL,mask INTEGER NOT NULL,reason TEXT NOT NULL);"
    "CREATE TABLE window_evidence(id INTEGER PRIMARY KEY REFERENCES operation(id) ON DELETE CASCADE,source_window INTEGER NOT NULL,"
    "source_run INTEGER NOT NULL,cutoff INTEGER NOT NULL,start INTEGER NOT NULL,end INTEGER NOT NULL,duration REAL NOT NULL,"
    "name TEXT NOT NULL,path TEXT NOT NULL,feedback INTEGER NOT NULL,feedback_expires INTEGER NOT NULL);"
    "CREATE TABLE metric_evidence(window_id INTEGER NOT NULL REFERENCES window_evidence(id) ON DELETE CASCADE,metric INTEGER NOT NULL,"
    "current REAL,median REAL,mad REAL,threshold REAL,covered REAL NOT NULL,n INTEGER NOT NULL,days INTEGER NOT NULL,minutes INTEGER NOT NULL,"
    "valid_samples INTEGER NOT NULL,state INTEGER NOT NULL,streak INTEGER NOT NULL,eligible INTEGER NOT NULL,reason TEXT NOT NULL,"
    "continuity INTEGER NOT NULL,PRIMARY KEY(window_id,metric)) WITHOUT ROWID;"
    "CREATE TABLE decision(id INTEGER PRIMARY KEY AUTOINCREMENT,window_id INTEGER NOT NULL REFERENCES window_evidence(id) ON DELETE CASCADE,"
    "metric INTEGER NOT NULL,method INTEGER NOT NULL,state INTEGER NOT NULL,streak INTEGER NOT NULL,threshold REAL,episode_id INTEGER,"
    "UNIQUE(window_id,metric,method));"
    "CREATE TABLE episode(id INTEGER PRIMARY KEY AUTOINCREMENT,session INTEGER NOT NULL,pid INTEGER NOT NULL,creation INTEGER NOT NULL,"
    "metric INTEGER NOT NULL,method INTEGER NOT NULL,state INTEGER NOT NULL,first INTEGER NOT NULL,activated INTEGER NOT NULL,"
    "last INTEGER NOT NULL,ended INTEGER NOT NULL,activation_decision INTEGER NOT NULL,last_decision INTEGER NOT NULL,"
    "qualifying INTEGER NOT NULL,name TEXT NOT NULL,path TEXT NOT NULL,reason TEXT NOT NULL);"
    "CREATE INDEX episode_key ON episode(session,pid,creation,metric,method,state);"
    "CREATE TABLE reduction(session INTEGER NOT NULL,pid INTEGER NOT NULL,creation INTEGER NOT NULL,metric INTEGER NOT NULL,method INTEGER NOT NULL,"
    "streak INTEGER NOT NULL,first INTEGER NOT NULL,episode INTEGER NOT NULL,PRIMARY KEY(session,pid,creation,metric,method)) WITHOUT ROWID;"
    "CREATE TABLE evaluation(id INTEGER PRIMARY KEY AUTOINCREMENT,episode INTEGER,decision INTEGER,trial TEXT NOT NULL,ordering TEXT NOT NULL,"
    "judgment INTEGER NOT NULL,understood INTEGER NOT NULL,target_correct INTEGER NOT NULL,time_correct INTEGER NOT NULL,"
    "metric_correct INTEGER NOT NULL,next_check INTEGER NOT NULL,elapsed REAL,note TEXT NOT NULL,created INTEGER NOT NULL);"
    "CREATE TABLE retention_state(expired_prefix INTEGER NOT NULL); INSERT INTO retention_state VALUES(0);"
    "CREATE TABLE replay_info(mode TEXT NOT NULL); INSERT INTO replay_info VALUES('capture');"
    "CREATE INDEX operation_time ON operation(utc); CREATE INDEX decision_window ON decision(window_id);";

char *exp_store_dup_text(const char *p)
{
    size_t n = strlen(p ? p : "") + 1;
    char *r = malloc(n);
    if (r)
        memcpy(r, p ? p : "", n);
    return r;
}
/* SQLite 소유 문자열이다. 다음 step/reset/finalize 전에 필요한 값을 복사해야 한다. */
const char *exp_store_column_text(sqlite3_stmt *s, int col)
{
    const unsigned char *p = sqlite3_column_text(s, col);
    return p ? (const char *)p : "";
}
void exp_store_db_error(Experiment *e, const char *what)
{
    snprintf(e->error, sizeof(e->error), "%s: %s (%d)", what, sqlite3_errmsg(e->db), sqlite3_extended_errcode(e->db));
}
int exp_store_execute(Experiment *e, const char *sql)
{
    if (sqlite3_exec(e->db, sql, NULL, NULL, NULL) == SQLITE_OK)
        return 1;
    exp_store_db_error(e, "experiment SQL");
    return 0;
}
int exp_store_prepare(Experiment *e, const char *sql, sqlite3_stmt **s)
{
    if (sqlite3_prepare_v2(e->db, sql, -1, s, NULL) == SQLITE_OK)
        return 1;
    exp_store_db_error(e, "experiment prepare");
    return 0;
}
int exp_store_done(Experiment *e, sqlite3_stmt *s)
{
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE)
        exp_store_db_error(e, "experiment step");
    return rc == SQLITE_DONE;
}
/* 수치 결측은 SQL NULL과 NAN 사이에서 보존해 유효한 0과 구분한다. */
void exp_store_bind_real(sqlite3_stmt *s, int col, double value)
{
    if (isfinite(value))
        sqlite3_bind_double(s, col, value);
    else
        sqlite3_bind_null(s, col);
}
double exp_store_real_column(sqlite3_stmt *s, int col)
{
    return sqlite3_column_type(s, col) == SQLITE_NULL ? NAN : sqlite3_column_double(s, col);
}
int exp_store_local_path(const char *path, wchar_t *full)
{
    wchar_t wide[32768], root[4];
    DWORD n;
    if (!path || !path[0] || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768))
        return 0;
    n = GetFullPathNameW(wide, 32768, full, NULL);
    if (!n || n >= 32768 || full[0] == L'\\' || full[1] != L':')
        return 0;
    root[0] = full[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = 0;
    return GetDriveTypeW(root) != DRIVE_REMOTE;
}
int exp_store_file_absent(const char *path)
{
    wchar_t full[32768];
    wchar_t companion[32768];
    const wchar_t *suffix[] = {L"", L"-wal", L"-shm", L"-journal"};
    unsigned i;
    if (!exp_store_local_path(path, full) || wcslen(full) > 32750)
        return 0;
    for (i = 0; i < 4; ++i) {
        _snwprintf_s(companion, 32768, _TRUNCATE, L"%s%s", full, suffix[i]);
        if (GetFileAttributesW(companion) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
            return 0;
    }
    return 1;
}
char *exp_store_absolute_utf8(const char *path)
{
    wchar_t full[32768];
    char *s;
    int n;
    if (!exp_store_local_path(path, full))
        return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, full, -1, NULL, 0, NULL, NULL);
    s = n ? malloc((size_t)n) : NULL;
    if (s)
        WideCharToMultiByte(CP_UTF8, 0, full, -1, s, n, NULL, NULL);
    return s;
}
void exp_store_copy_error(Experiment *e, char *error, size_t size)
{
    if (error && size)
        snprintf(error, size, "%s", e ? e->error : "Out of memory");
}
static int validate_db(Experiment *e)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!exp_store_prepare(
            e,
            "SELECT (SELECT application_id FROM pragma_application_id),(SELECT user_version FROM pragma_user_version)",
            &s))
        return 0;
    ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == EXP_APP && sqlite3_column_int(s, 1) == 1;
    sqlite3_finalize(s);
    s = NULL;
    if (!ok) {
        snprintf(e->error, sizeof(e->error), "Not a supported experiment evidence DB; no migration performed");
        return 0;
    }
    if (!exp_store_prepare(
            e,
            "SELECT pair,source,synthetic,policy,template,replay,retention_days,cpu_limit,ram_limit,read_limit,write_limit FROM experiment",
            &s))
        return 0;
    ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) {
        e->pair = exp_store_dup_text(exp_store_column_text(s, 0));
        e->source = exp_store_dup_text(exp_store_column_text(s, 1));
        e->synthetic = sqlite3_column_int(s, 2);
        e->replay = sqlite3_column_int(s, 5);
        ok = e->pair && e->source && strlen(e->pair) == 32 && (e->synthetic == 0 || e->synthetic == 1) &&
             strcmp(exp_store_column_text(s, 3), EXPERIMENT_POLICY_VERSION) == 0 &&
             strcmp(exp_store_column_text(s, 4), EXPERIMENT_TEMPLATE_VERSION) == 0 &&
             sqlite3_column_int(s, 6) == EXPERIMENT_RETENTION_DAYS && sqlite3_column_double(s, 7) == exp_fixed_limits[0] &&
             sqlite3_column_double(s, 8) == exp_fixed_limits[1] && sqlite3_column_double(s, 9) == exp_fixed_limits[2] &&
             sqlite3_column_double(s, 10) == exp_fixed_limits[3];
        if (sqlite3_step(s) != SQLITE_DONE)
            ok = 0;
    }
    sqlite3_finalize(s);
    if (!ok)
        snprintf(e->error, sizeof(e->error), "Invalid or unsupported experiment policy/configuration");
    if (ok) {
        if (!exp_store_prepare(e, "SELECT expired_prefix FROM retention_state", &s))
            return 0;
        ok = sqlite3_step(s) == SQLITE_ROW;
        if (ok)
            e->expired_prefix = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return ok;
}
int exp_store_open_sidecar(const char *path, int writable, Experiment **out, char *error, size_t size)
{
    Experiment *e = calloc(1, sizeof(*e));
    wchar_t full[32768];
    if (out)
        *out = NULL;
    if (!out || !e) {
        free(e);
        return 0;
    }
    if (!exp_store_local_path(path, full)) {
        snprintf(e->error, sizeof(e->error), "Local UTF-8 experiment path required");
        goto failed;
    }
    e->readonly = !writable;
    if (sqlite3_open_v2(path, &e->db, writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        exp_store_db_error(e, "open evidence");
        goto failed;
    }
    sqlite3_busy_timeout(e->db, 250);
    sqlite3_extended_result_codes(e->db, 1);
    if (!validate_db(e) || !exp_store_execute(e, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF;"))
        goto failed;
    *out = e;
    return 1;
failed:
    exp_store_copy_error(e, error, size);
    experiment_close(e);
    return 0;
}
int exp_store_record_operation(Experiment *e, int kind, uint64_t utc, uint64_t session, uint32_t pid, uint64_t creation,
                            unsigned mask, const char *reason, int64_t *id)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (utc > INT64_MAX || session > INT64_MAX || creation > INT64_MAX) {
        snprintf(e->error, sizeof(e->error), "Experiment timestamp outside SQLite range");
        return 0;
    }
    if (!exp_store_prepare(e, "INSERT INTO operation(kind,utc,session,pid,creation,mask,reason) VALUES(?,?,?,?,?,?,?)", &s))
        return 0;
    sqlite3_bind_int(s, 1, kind);
    sqlite3_bind_int64(s, 2, (int64_t)utc);
    sqlite3_bind_int64(s, 3, (int64_t)session);
    sqlite3_bind_int64(s, 4, pid);
    sqlite3_bind_int64(s, 5, (int64_t)creation);
    sqlite3_bind_int(s, 6, (int)mask);
    sqlite3_bind_text(s, 7, reason ? reason : "", -1, SQLITE_TRANSIENT);
    ok = exp_store_done(e, s);
    sqlite3_finalize(s);
    if (ok && id)
        *id = sqlite3_last_insert_rowid(e->db);
    return ok;
}
int exp_store_metric(Experiment *e, int64_t window, unsigned metric, const MonitorMetricReport *r, int continuity)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!exp_store_prepare(e, "INSERT INTO metric_evidence VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &s))
        return 0;
    sqlite3_bind_int64(s, 1, window);
    sqlite3_bind_int(s, 2, (int)metric);
    exp_store_bind_real(s, 3, r->current);
    exp_store_bind_real(s, 4, r->median);
    exp_store_bind_real(s, 5, r->mad);
    exp_store_bind_real(s, 6, r->threshold);
    sqlite3_bind_double(s, 7, r->covered_seconds);
    sqlite3_bind_int(s, 8, (int)r->count);
    sqlite3_bind_int(s, 9, (int)r->days);
    sqlite3_bind_int(s, 10, (int)r->minutes);
    sqlite3_bind_int(s, 11, (int)r->valid_samples);
    sqlite3_bind_int(s, 12, r->state);
    sqlite3_bind_int(s, 13, (int)r->streak);
    sqlite3_bind_int(s, 14, r->baseline_eligible);
    sqlite3_bind_text(s, 15, r->reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 16, continuity);
    ok = exp_store_done(e, s);
    sqlite3_finalize(s);
    return ok;
}
int exp_store_initialize_schema(Experiment *e)
{
    return exp_store_execute(e, schema);
}

int exp_store_window(Experiment *e, int64_t source_window, int64_t source_run, int64_t cutoff, uint64_t session,
                        uint32_t pid, uint64_t creation, const char *name, const char *path,
                        const MonitorMetricReport *reports, int feedback, uint64_t feedback_expires,
                        int64_t *stored_id)
{
    sqlite3_stmt *s = NULL;
    int64_t id;
    if (!exp_store_record_operation(e, 2, reports[0].window_end_utc_100ns, session, pid, creation, 0,
                          "completed shared observation window", &id))
        return 0;
    if (!exp_store_prepare(e, "INSERT INTO window_evidence VALUES(?,?,?,?,?,?,?,?,?,?,?)", &s))
        return 0;
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_int64(s, 2, source_window);
    sqlite3_bind_int64(s, 3, source_run);
    sqlite3_bind_int64(s, 4, cutoff);
    sqlite3_bind_int64(s, 5, (int64_t)reports[0].window_start_utc_100ns);
    sqlite3_bind_int64(s, 6, (int64_t)reports[0].window_end_utc_100ns);
    sqlite3_bind_double(s, 7, reports[0].window_duration_seconds);
    sqlite3_bind_text(s, 8, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 9, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 10, feedback);
    sqlite3_bind_int64(s, 11, (int64_t)feedback_expires);
    if (!exp_store_done(e, s)) {
        sqlite3_finalize(s);
        return 0;
    }
    sqlite3_finalize(s);
    *stored_id = id;
    return 1;
}
