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
#include "monitor_schema.h"
#define APP_ID 1297042993

void monitor_db_set_error(MonitorEngine *e, const char *what)
{
    snprintf(e->error, sizeof(e->error), "%s: %s (%d)", what,
        sqlite3_errmsg(e->db), sqlite3_extended_errcode(e->db));
}
int monitor_db_execute(MonitorEngine *e, const char *sql)
{
    if (sqlite3_exec(e->db, sql, NULL, NULL, NULL) == SQLITE_OK) return 1;
    monitor_db_set_error(e, sql); return 0;
}
int monitor_db_prepare(MonitorEngine *e, const char *sql, sqlite3_stmt **s)
{
    if (sqlite3_prepare_v2(e->db, sql, -1, s, NULL) == SQLITE_OK) return 1;
    monitor_db_set_error(e, "prepare"); return 0;
}
int monitor_db_done(MonitorEngine *e, sqlite3_stmt *s)
{
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) monitor_db_set_error(e, "step");
    sqlite3_reset(s); sqlite3_clear_bindings(s);
    return rc == SQLITE_DONE;
}
int64_t monitor_db_scalar(MonitorEngine *e, const char *sql, int *ok)
{
    sqlite3_stmt *s = NULL;
    int64_t value = 0;
    *ok = 0;
    if (!monitor_db_prepare(e, sql, &s)) return 0;
    if (sqlite3_step(s) == SQLITE_ROW) { value = sqlite3_column_int64(s, 0); *ok = 1; }
    else monitor_db_set_error(e, "scalar");
    sqlite3_finalize(s); return value;
}
const char *monitor_error(const MonitorEngine *e) { return e ? e->error : "engine unavailable"; }
const char *monitor_storage_version(void) { return sqlite3_libversion(); }
int monitor_storage_stats(MonitorEngine *e, MonitorStorageStats *out)
{
    int ok;
    if (!e || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->raw_rows = (uint64_t)monitor_db_scalar(e, "SELECT count(*) FROM raw", &ok); if (!ok) return 0;
    out->windows = (uint64_t)monitor_db_scalar(e, "SELECT count(*) FROM aggregate_window", &ok); if (!ok) return 0;
    out->metric_rows = (uint64_t)monitor_db_scalar(e, "SELECT count(*) FROM aggregate_metric", &ok); if (!ok) return 0;
    out->database_page_bytes = (uint64_t)monitor_db_scalar(e, "SELECT page_count*page_size FROM pragma_page_count(),pragma_page_size()", &ok); if (!ok) return 0;
    out->raw_page_bytes = (uint64_t)monitor_db_scalar(e, "SELECT COALESCE(sum(pgsize),0) FROM dbstat WHERE name IN('raw','raw_time','raw_run','raw_program_time')", &ok); if (!ok) return 0;
    out->aggregate_page_bytes = (uint64_t)monitor_db_scalar(e, "SELECT COALESCE(sum(pgsize),0) FROM dbstat WHERE name IN('aggregate_window','aggregate_time','aggregate_run','aggregate_program_time','aggregate_metric','metric_baseline')", &ok);
    return ok;
}
static wchar_t *wide_path(const char *path)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) {
        free(wide); wide = NULL;
    }
    return wide;
}
static int check_local_path(const wchar_t *path)
{
    wchar_t full[32768], root[4];
    DWORD n = GetFullPathNameW(path, 32768, full, NULL);
    if (!n || n >= 32768 || full[0] == L'\\' || full[1] != L':') return 0;
    root[0] = full[0]; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
    return GetDriveTypeW(root) != DRIVE_REMOTE;
}
int monitor_open(const char *path, int synthetic, MonitorEngine **out, char *error, size_t size)
{
    MonitorEngine *e = NULL;
    wchar_t *wide = NULL;
    sqlite3_stmt *s = NULL;
    int exists, ok = 0;
    int64_t version, appid;
    if (out) *out = NULL;
    if (error && size) error[0] = 0;
    if (!out || !path || !path[0] || (synthetic != 0 && synthetic != 1)) return 0;
    wide = wide_path(path);
    if (!wide || !check_local_path(wide)) {
        if (error && size) snprintf(error, size, "A local disk UTF-8 DB path is required");
        free(wide); return 0;
    }
    exists = GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES; free(wide);
    e = calloc(1, sizeof(*e));
    if (!e) return 0;
    e->synthetic = synthetic;
    if (sqlite3_open_v2(path, &e->db, SQLITE_OPEN_READWRITE | (exists ? 0 : SQLITE_OPEN_CREATE), NULL) != SQLITE_OK) {
        monitor_db_set_error(e, "open"); goto failed;
    }
    sqlite3_extended_result_codes(e->db, 1);
    sqlite3_busy_timeout(e->db, 250);
    version = monitor_db_scalar(e, "PRAGMA user_version", &ok); if (!ok) goto failed;
    appid = monitor_db_scalar(e, "PRAGMA application_id", &ok); if (!ok) goto failed;
    if (exists && (version != 1 || appid != APP_ID)) {
        snprintf(e->error, sizeof(e->error), "Unknown or unsupported DB schema (app=%lld version=%lld)",
            (long long)appid, (long long)version); goto failed;
    }
    if (!exists) {
        if (!monitor_db_execute(e, "BEGIN IMMEDIATE") || !monitor_db_execute(e, monitor_schema)) goto failed;
        if (!monitor_db_prepare(e, "INSERT INTO metadata VALUES(?)", &s)) goto failed;
        sqlite3_bind_int(s, 1, synthetic);
        if (!monitor_db_done(e, s)) goto failed;
        sqlite3_finalize(s); s = NULL;
        if (!monitor_db_execute(e, "COMMIT")) goto failed;
    }
    if (monitor_db_scalar(e, "SELECT synthetic FROM metadata", &ok) != synthetic || !ok) {
        snprintf(e->error, sizeof(e->error), "Real/SYNTHETIC DB mismatch or invalid metadata"); goto failed;
    }
    if (!monitor_db_execute(e, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA trusted_schema=OFF; PRAGMA main.cache_size=-16384")) goto failed;
    if (!monitor_db_prepare(e, "INSERT INTO session(started) VALUES(?)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)monitor_utc_now());
    if (!monitor_db_done(e, s)) goto failed;
    sqlite3_finalize(s); s = NULL; e->session = sqlite3_last_insert_rowid(e->db);
    if (!monitor_db_prepare(e, "SELECT id FROM program WHERE path=?", &e->program_find) ||
        !monitor_db_prepare(e, "INSERT INTO program(path,name,first_seen,last_seen) VALUES(?,?,?,?)", &e->program_insert) ||
        !monitor_db_prepare(e, "INSERT INTO raw(run_id,utc,pid,creation,program_id,valid,cpu,ram,io_read,io_write,elapsed,observation_interval,cpu_interval,io_interval,foreground,cpu_status,read_status,write_status,open_error,times_error,memory_error,io_error,name_error,path_error,name) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &e->raw_insert) ||
        !monitor_db_prepare(e,
            "WITH RECURSIVE picks(id,end_utc,value,n) AS ("
            "SELECT NULL,?4,NULL,0 UNION ALL "
            "SELECT w.id,w.end_utc,(SELECT value FROM aggregate_metric WHERE window_id=w.id AND metric=?1),p.n+1 "
            "FROM picks p JOIN aggregate_window w ON w.id=("
            "SELECT x.id FROM aggregate_window x JOIN aggregate_metric m ON m.window_id=x.id AND m.metric=?1 "
            "AND m.eligible=1 AND m.value IS NOT NULL AND m.value>=0 AND m.value<=1.7976931348623157e308 "
            "WHERE x.program_id=?2 AND x.version=1 AND x.end_utc>=?3 "
            "AND x.end_utc<min((p.end_utc/600000000)*600000000,?5) AND x.id<=?6 "
            "ORDER BY x.end_utc DESC,x.id DESC LIMIT 1) WHERE p.n<64) "
            "SELECT value,end_utc FROM picks WHERE n>0", &e->baseline_query)) goto failed;
    *out = e; return 1;
failed:
    if (error && size) snprintf(error, size, "%s", e->error);
    sqlite3_finalize(s);
    if (e->db) sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    monitor_close(e); return 0;
}
void monitor_close(MonitorEngine *e)
{
    sqlite3_stmt *s = NULL;
    if (!e) return;
    monitor_reset(e);
    sqlite3_finalize(e->raw_insert); sqlite3_finalize(e->program_find);
    sqlite3_finalize(e->program_insert); sqlite3_finalize(e->baseline_query);
    if (e->db && e->session && sqlite3_prepare_v2(e->db, "UPDATE session SET ended=? WHERE id=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)monitor_utc_now()); sqlite3_bind_int64(s, 2, e->session);
        sqlite3_step(s); sqlite3_finalize(s);
    }
    sqlite3_close(e->db); free(e);
}
int64_t monitor_db_program_id(MonitorEngine *e, const char *path, const char *name, uint64_t utc)
{
    sqlite3_int64 id = 0;
    int rc;
    sqlite3_stmt *s = e->program_find;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT); rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) id = sqlite3_column_int64(s, 0);
    else if (rc != SQLITE_DONE) monitor_db_set_error(e, "find program");
    sqlite3_reset(s); sqlite3_clear_bindings(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return 0;
    if (id) {
        sqlite3_stmt *update = NULL;
        if (!monitor_db_prepare(e, "UPDATE program SET last_seen=max(last_seen,?) WHERE id=?", &update)) return 0;
        sqlite3_bind_int64(update, 1, (sqlite3_int64)utc); sqlite3_bind_int64(update, 2, id);
        if (!monitor_db_done(e, update)) { sqlite3_finalize(update); return 0; }
        sqlite3_finalize(update); return id;
    }
    s = e->program_insert;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, name ? name : "(unavailable)", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)utc); sqlite3_bind_int64(s, 4, (sqlite3_int64)utc);
    return monitor_db_done(e, s) ? sqlite3_last_insert_rowid(e->db) : 0;
}
void monitor_db_bind_value(sqlite3_stmt *s, int index, int valid, double value)
{
    if (valid && isfinite(value)) sqlite3_bind_double(s, index, value);
    else sqlite3_bind_null(s, index);
}
int monitor_db_save_raw(MonitorEngine *e, sqlite3_int64 run, const ResourceData *r, const MonitorInstance *p)
{
    sqlite3_stmt *s = e->raw_insert;
    unsigned v = r->valid_fields;
    sqlite3_bind_int64(s, 1, run); sqlite3_bind_int64(s, 2, (sqlite3_int64)r->measured_at_utc_100ns);
    sqlite3_bind_int64(s, 3, r->pid);
    if (v & RESOURCE_VALID_IDENTITY) sqlite3_bind_int64(s, 4, (sqlite3_int64)r->creation_time_utc_100ns);
    if (p->program_id) sqlite3_bind_int64(s, 5, p->program_id);
    sqlite3_bind_int64(s, 6, v);
    monitor_db_bind_value(s, 7, v & RESOURCE_VALID_CPU, r->cpu_percent);
    if (v & RESOURCE_VALID_MEMORY) sqlite3_bind_int64(s, 8, (sqlite3_int64)r->working_set_bytes);
    monitor_db_bind_value(s, 9, v & RESOURCE_VALID_IO_READ, r->io_read_bytes_per_second);
    monitor_db_bind_value(s, 10, v & RESOURCE_VALID_IO_WRITE, r->io_write_bytes_per_second);
    monitor_db_bind_value(s, 11, v & RESOURCE_VALID_ELAPSED, r->elapsed_seconds);
    monitor_db_bind_value(s, 12, v & RESOURCE_VALID_OBSERVATION_INTERVAL, r->observation_interval_seconds);
    monitor_db_bind_value(s, 13, v & RESOURCE_VALID_CPU, r->cpu_interval_seconds);
    monitor_db_bind_value(s, 14, v & (RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE), r->io_interval_seconds);
    if (v & RESOURCE_VALID_FOREGROUND) sqlite3_bind_int(s, 15, r->foreground);
    sqlite3_bind_int(s, 16, r->cpu_status); sqlite3_bind_int(s, 17, r->io_read_status); sqlite3_bind_int(s, 18, r->io_write_status);
    sqlite3_bind_int64(s, 19, r->open_error); sqlite3_bind_int64(s, 20, r->times_error);
    sqlite3_bind_int64(s, 21, r->memory_error); sqlite3_bind_int64(s, 22, r->io_error);
    sqlite3_bind_int64(s, 23, r->name_error); sqlite3_bind_int64(s, 24, p->path_error);
    if ((v & RESOURCE_VALID_NAME) && r->process_name_utf8) sqlite3_bind_text(s, 25, r->process_name_utf8, -1, SQLITE_TRANSIENT);
    return monitor_db_done(e, s);
}
int monitor_db_get_feedback(MonitorEngine *e, MonitorRow *r, uint64_t now)
{
    sqlite3_stmt *s = NULL;
    int rc;
    if (!r->program_id) return 1;
    if (!monitor_db_prepare(e, "SELECT kind,expires FROM feedback WHERE program_id=? AND cancelled=0 AND kind<>0 AND expires>? ORDER BY id DESC LIMIT 1", &s)) return 0;
    sqlite3_bind_int64(s, 1, r->program_id); sqlite3_bind_int64(s, 2, (sqlite3_int64)now);
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        r->feedback = (MonitorFeedback)sqlite3_column_int(s, 0);
        r->feedback_expires_utc_100ns = (uint64_t)sqlite3_column_int64(s, 1);
    } else if (rc != SQLITE_DONE) monitor_db_set_error(e, "feedback read");
    sqlite3_finalize(s); return rc == SQLITE_ROW || rc == SQLITE_DONE;
}
int monitor_db_trim(MonitorEngine *e, uint64_t now)
{
    static const char *const statements[] = {
        "DELETE FROM raw WHERE id IN(SELECT id FROM raw WHERE utc<? ORDER BY utc LIMIT 2000)",
        "DELETE FROM aggregate_window WHERE id IN(SELECT id FROM aggregate_window WHERE end_utc<? ORDER BY end_utc LIMIT 1000)",
        "DELETE FROM collection_run WHERE id IN(SELECT id FROM collection_run WHERE finished<? AND NOT EXISTS(SELECT 1 FROM aggregate_window w WHERE w.run_id=collection_run.id) AND NOT EXISTS(SELECT 1 FROM raw r WHERE r.run_id=collection_run.id) ORDER BY finished LIMIT 1000)",
        "DELETE FROM feedback WHERE id IN(SELECT id FROM feedback WHERE expires<? ORDER BY expires LIMIT 1000)",
        "DELETE FROM session WHERE id IN(SELECT id FROM session WHERE id<>? AND started<? AND NOT EXISTS(SELECT 1 FROM collection_run r WHERE r.session_id=session.id) ORDER BY started LIMIT 1000)",
        "DELETE FROM program WHERE id IN(SELECT id FROM program WHERE last_seen<? AND NOT EXISTS(SELECT 1 FROM raw r WHERE r.program_id=program.id) AND NOT EXISTS(SELECT 1 FROM aggregate_window w WHERE w.program_id=program.id) AND NOT EXISTS(SELECT 1 FROM feedback f WHERE f.program_id=program.id) LIMIT 128)"
    };
    unsigned i;
    for (i = 0; i < sizeof(statements) / sizeof(statements[0]); ++i) {
        sqlite3_stmt *s = NULL;
        uint64_t retention = i == 0 ? 3600 * TICKS : (i == 3 ? 7 * DAY : 28 * DAY);
        if (!monitor_db_prepare(e, statements[i], &s)) return 0;
        if (i == 4) { sqlite3_bind_int64(s, 1, e->session); sqlite3_bind_int64(s, 2, (sqlite3_int64)(now > retention ? now - retention : 0)); }
        else sqlite3_bind_int64(s, 1, (sqlite3_int64)(now > retention ? now - retention : 0));
        if (!monitor_db_done(e, s)) { sqlite3_finalize(s); return 0; }
        sqlite3_finalize(s);
    }
    return 1;
}
int monitor_record_failure(MonitorEngine *e, uint64_t utc, unsigned status)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!e || utc > INT64_MAX) return 0;
    monitor_reset(e);
    if (!monitor_db_prepare(e, "INSERT INTO collection_run(session_id,started,finished,status,row_count,cpu_count) VALUES(?,?,?,?,0,0)", &s)) return 0;
    sqlite3_bind_int64(s, 1, e->session); sqlite3_bind_int64(s, 2, (sqlite3_int64)utc);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)utc); sqlite3_bind_int64(s, 4, status ? status : 1);
    ok = monitor_db_done(e, s); sqlite3_finalize(s); return ok;
}
int monitor_feedback(MonitorEngine *e, int64_t program, MonitorFeedback kind, uint64_t now)
{
    sqlite3_stmt *s = NULL;
    uint64_t duration = kind == MONITOR_FEEDBACK_IGNORE ? 3600 * TICKS : 86400 * TICKS;
    if (!e || program <= 0 || kind < MONITOR_FEEDBACK_CANCEL || kind > MONITOR_FEEDBACK_EXPECTED || now > INT64_MAX - duration) return 0;
    if (!monitor_db_execute(e, "BEGIN IMMEDIATE")) return 0;
    if (!monitor_db_prepare(e, "UPDATE feedback SET cancelled=1 WHERE program_id=? AND cancelled=0", &s)) goto failed;
    sqlite3_bind_int64(s, 1, program); if (!monitor_db_done(e, s)) goto failed;
    sqlite3_finalize(s); s = NULL;
    if (!monitor_db_prepare(e, "INSERT INTO feedback(program_id,kind,created,expires) VALUES(?,?,?,?)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, program); sqlite3_bind_int(s, 2, kind);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)now); sqlite3_bind_int64(s, 4, (sqlite3_int64)(kind ? now + duration : now));
    if (!monitor_db_done(e, s)) goto failed;
    sqlite3_finalize(s); s = NULL;
    if (!monitor_db_trim(e, now) || !monitor_db_execute(e, "COMMIT")) goto failed;
    return 1;
failed:
    sqlite3_finalize(s); sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL); return 0;
}
