/* 로컬 SQLite DB의 개방·검사와 원시/집계/피드백 저장, 보관 정리를 담당한다.
 * 수집 회차의 트랜잭션 경계는 engine이 결정하며 이 모듈은 SQL과 statement를 관리한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "monitor_store_private.h"
#include "monitor.h"

#define TICKS MONITOR_TICKS_PER_SECOND
#define DAY MONITOR_DAY_TICKS
#include "monitor_schema.h"
#define APP_ID 1297042993



void monitor_store_set_error(MonitorStore *e, const char *what)
{
    snprintf(e->error, sizeof(e->error), "%s: %s (%d)", what,
        sqlite3_errmsg(e->db), sqlite3_extended_errcode(e->db));
}


int monitor_store_execute(MonitorStore *e, const char *sql)
{
    if (sqlite3_exec(e->db, sql, NULL, NULL, NULL) == SQLITE_OK) return 1;
    monitor_store_set_error(e, sql);
    return 0;
}


int monitor_store_prepare(MonitorStore *e, const char *sql, sqlite3_stmt **s)
{
    if (sqlite3_prepare_v2(e->db, sql, -1, s, NULL) == SQLITE_OK) return 1;
    monitor_store_set_error(e, "prepare");
    return 0;
}


int monitor_store_done(MonitorStore *e, sqlite3_stmt *s)
{
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) monitor_store_set_error(e, "step");
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    return rc == SQLITE_DONE;
}


int64_t monitor_store_scalar(MonitorStore *e, const char *sql, int *ok)
{
    sqlite3_stmt *s = NULL;
    int64_t value = 0;
    *ok = 0;
    if (!monitor_store_prepare(e, sql, &s)) return 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        value = sqlite3_column_int64(s, 0);
        *ok = 1;
    } else monitor_store_set_error(e, "scalar");
    sqlite3_finalize(s);
    return value;
}


const char *monitor_storage_version(void)
{
    return sqlite3_libversion();
}


int monitor_store_stats(MonitorStore *e, MonitorStorageStats *out)
{
    int ok;
    if (!e || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->raw_rows = (uint64_t)monitor_store_scalar(e, "SELECT count(*) FROM raw", &ok);
    if (!ok) return 0;
    out->windows = (uint64_t)monitor_store_scalar(e, "SELECT count(*) FROM aggregate_window", &ok);
    if (!ok) return 0;
    out->metric_rows = (uint64_t)monitor_store_scalar(e, "SELECT count(*) FROM aggregate_metric", &ok);
    if (!ok) return 0;
    out->database_page_bytes = (uint64_t)monitor_store_scalar(e, "SELECT page_count*page_size FROM pragma_page_count(),pragma_page_size()", &ok);
    if (!ok) return 0;
    out->raw_page_bytes = (uint64_t)monitor_store_scalar(e, "SELECT COALESCE(sum(pgsize),0) FROM dbstat WHERE name IN('raw','raw_time','raw_run','raw_program_time')", &ok);
    if (!ok) return 0;
    out->aggregate_page_bytes = (uint64_t)monitor_store_scalar(e, "SELECT COALESCE(sum(pgsize),0) FROM dbstat WHERE name IN('aggregate_window','aggregate_time','aggregate_run','aggregate_program_time','aggregate_metric','metric_baseline')", &ok);
    return ok;
}


static wchar_t *wide_path(const char *path)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) {
        free(wide);
        wide = NULL;
    }
    return wide;
}


static int check_local_path(const wchar_t *path)
{
    wchar_t full[32768], root[4];
    DWORD n = GetFullPathNameW(path, 32768, full, NULL);
    if (!n || n >= 32768 || full[0] == L'\\' || full[1] != L':') return 0;
    root[0] = full[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = 0;
    return GetDriveTypeW(root) != DRIVE_REMOTE;
}


static int open_schema(MonitorStore *e, int exists, int synthetic,
    sqlite3_stmt **statement)
{
    int ok = 0;
    int64_t version, appid;
    version = monitor_store_scalar(e, "PRAGMA user_version", &ok);
    if (!ok) return 0;
    appid = monitor_store_scalar(e, "PRAGMA application_id", &ok);
    if (!ok) return 0;
    if (exists && (version != 1 || appid != APP_ID)) {
        snprintf(e->error, sizeof(e->error), "Unknown or unsupported DB schema (app=%lld version=%lld)",
            (long long)appid, (long long)version);
        return 0;
    }
    if (!exists) {
        if (!monitor_store_execute(e, "BEGIN IMMEDIATE") || !monitor_store_execute(e, monitor_schema)) return 0;
        if (!monitor_store_prepare(e, "INSERT INTO metadata VALUES(?)", statement)) return 0;
        sqlite3_bind_int(*statement, 1, synthetic);
        if (!monitor_store_done(e, *statement)) return 0;
        sqlite3_finalize(*statement);
        *statement = NULL;
        if (!monitor_store_execute(e, "COMMIT")) return 0;
    }
    if (monitor_store_scalar(e, "SELECT synthetic FROM metadata", &ok) != synthetic || !ok) {
        snprintf(e->error, sizeof(e->error), "Real/SYNTHETIC DB mismatch or invalid metadata");
        return 0;
    }
    return 1;
}


static int start_session(MonitorStore *e, sqlite3_stmt **statement)
{
    if (!monitor_store_execute(e, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA trusted_schema=OFF; PRAGMA main.cache_size=-16384")) return 0;
    if (!monitor_store_prepare(e, "INSERT INTO session(started) VALUES(?)", statement)) return 0;
    sqlite3_bind_int64(*statement, 1, (sqlite3_int64)monitor_utc_now());
    if (!monitor_store_done(e, *statement)) return 0;
    sqlite3_finalize(*statement);
    *statement = NULL;
    e->session = sqlite3_last_insert_rowid(e->db);
    return 1;
}


static int prepare_store_statements(MonitorStore *e)
{
    if (!monitor_store_prepare(e, "SELECT id FROM program WHERE path=?", &e->program_find) ||
        !monitor_store_prepare(e, "INSERT INTO program(path,name,first_seen,last_seen) VALUES(?,?,?,?)", &e->program_insert) ||
        !monitor_store_prepare(e, "INSERT INTO raw(run_id,utc,pid,creation,program_id,valid,cpu,ram,io_read,io_write,elapsed,observation_interval,cpu_interval,io_interval,foreground,cpu_status,read_status,write_status,open_error,times_error,memory_error,io_error,name_error,path_error,name) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &e->raw_insert) ||
        !monitor_history_prepare(e)) return 0;
    return 1;
}


int monitor_store_open(const char *path, int synthetic, MonitorStore **out, char *error, size_t size)
{
    MonitorStore *e = NULL;
    wchar_t *wide = NULL;
    sqlite3_stmt *s = NULL;
    int exists;
    if (out) *out = NULL;
    if (error && size) error[0] = 0;
    if (!out || !path || !path[0] || (synthetic != 0 && synthetic != 1)) return 0;
    wide = wide_path(path);
    if (!wide || !check_local_path(wide)) {
        if (error && size) snprintf(error, size, "A local disk UTF-8 DB path is required");
        free(wide);
        return 0;
    }
    exists = GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
    free(wide);
    e = calloc(1, sizeof(*e));
    if (!e) {
        if (error && size) snprintf(error, size, "Out of memory opening store");
        return 0;
    }
    if (sqlite3_open_v2(path, &e->db, SQLITE_OPEN_READWRITE | (exists ? 0 : SQLITE_OPEN_CREATE), NULL) != SQLITE_OK) {
        monitor_store_set_error(e, "open");
        goto failed;
    }
    sqlite3_extended_result_codes(e->db, 1);
    sqlite3_busy_timeout(e->db, 250);

    if (!open_schema(e, exists, synthetic, &s)) goto failed;
    if (!start_session(e, &s)) goto failed;
    if (!prepare_store_statements(e)) goto failed;

    *out = e;
    return 1;
failed:
    if (error && size) snprintf(error, size, "%s", e->error);
    sqlite3_finalize(s);
    if (e->db) sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    monitor_store_close(e);
    return 0;
}


void monitor_store_close(MonitorStore *e)
{
    sqlite3_stmt *s = NULL;
    if (!e) return;
    sqlite3_finalize(e->raw_insert);
    sqlite3_finalize(e->program_find);
    sqlite3_finalize(e->program_insert);
    sqlite3_finalize(e->baseline_query);
    if (e->db && e->session && sqlite3_prepare_v2(e->db, "UPDATE session SET ended=? WHERE id=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)monitor_utc_now());
        sqlite3_bind_int64(s, 2, e->session);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_close(e->db);
    free(e);
}


int64_t monitor_store_program_id(MonitorStore *e, const char *path, const char *name, uint64_t utc)
{
    sqlite3_int64 id = 0;
    int rc;
    sqlite3_stmt *s = e->program_find;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) id = sqlite3_column_int64(s, 0);
    else if (rc != SQLITE_DONE) monitor_store_set_error(e, "find program");
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return 0;
    if (id) {
        sqlite3_stmt *update = NULL;
        if (!monitor_store_prepare(e, "UPDATE program SET last_seen=max(last_seen,?) WHERE id=?", &update)) return 0;
        sqlite3_bind_int64(update, 1, (sqlite3_int64)utc);
        sqlite3_bind_int64(update, 2, id);
        if (!monitor_store_done(e, update)) {
            sqlite3_finalize(update);
            return 0;
        }
        sqlite3_finalize(update);
        return id;
    }
    s = e->program_insert;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, name ? name : "(unavailable)", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)utc);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)utc);
    return monitor_store_done(e, s) ? sqlite3_last_insert_rowid(e->db) : 0;
}


/* 유효하지 않거나 유한하지 않은 수치는 0이 아닌 SQL NULL로 저장한다. */
void monitor_store_bind_value(sqlite3_stmt *s, int index, int valid, double value)
{
    if (valid && isfinite(value)) sqlite3_bind_double(s, index, value);
    else sqlite3_bind_null(s, index);
}


int monitor_store_write_raw(MonitorStore *e, sqlite3_int64 run, const ResourceData *r, int64_t program_id, uint32_t path_error)
{
    sqlite3_stmt *s = e->raw_insert;
    unsigned v = r->valid_fields;
    sqlite3_bind_int64(s, 1, run);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)r->measured_at_utc_100ns);
    sqlite3_bind_int64(s, 3, r->pid);
    if (v & RESOURCE_VALID_IDENTITY) sqlite3_bind_int64(s, 4, (sqlite3_int64)r->creation_time_utc_100ns);
    if (program_id) sqlite3_bind_int64(s, 5, program_id);
    sqlite3_bind_int64(s, 6, v);
    monitor_store_bind_value(s, 7, v & RESOURCE_VALID_CPU, r->cpu_percent);
    if (v & RESOURCE_VALID_MEMORY) sqlite3_bind_int64(s, 8, (sqlite3_int64)r->working_set_bytes);
    monitor_store_bind_value(s, 9, v & RESOURCE_VALID_IO_READ, r->io_read_bytes_per_second);
    monitor_store_bind_value(s, 10, v & RESOURCE_VALID_IO_WRITE, r->io_write_bytes_per_second);
    monitor_store_bind_value(s, 11, v & RESOURCE_VALID_ELAPSED, r->elapsed_seconds);
    monitor_store_bind_value(s, 12, v & RESOURCE_VALID_OBSERVATION_INTERVAL, r->observation_interval_seconds);
    monitor_store_bind_value(s, 13, v & RESOURCE_VALID_CPU, r->cpu_interval_seconds);
    monitor_store_bind_value(s, 14, v & (RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE), r->io_interval_seconds);
    if (v & RESOURCE_VALID_FOREGROUND) sqlite3_bind_int(s, 15, r->foreground);
    sqlite3_bind_int(s, 16, r->cpu_status);
    sqlite3_bind_int(s, 17, r->io_read_status);
    sqlite3_bind_int(s, 18, r->io_write_status);
    sqlite3_bind_int64(s, 19, r->open_error);
    sqlite3_bind_int64(s, 20, r->times_error);
    sqlite3_bind_int64(s, 21, r->memory_error);
    sqlite3_bind_int64(s, 22, r->io_error);
    sqlite3_bind_int64(s, 23, r->name_error);
    sqlite3_bind_int64(s, 24, path_error);
    if ((v & RESOURCE_VALID_NAME) && r->process_name_utf8) sqlite3_bind_text(s, 25, r->process_name_utf8, -1, SQLITE_TRANSIENT);
    return monitor_store_done(e, s);
}


int monitor_store_read_feedback(MonitorStore *e, int64_t program_id, uint64_t now, MonitorFeedback *feedback, uint64_t *expires)
{
    sqlite3_stmt *s = NULL;
    int rc;
    if (!program_id) return 1;
    if (!monitor_store_prepare(e, "SELECT kind,expires FROM feedback WHERE program_id=? AND cancelled=0 AND kind<>0 AND expires>? ORDER BY id DESC LIMIT 1", &s)) return 0;
    sqlite3_bind_int64(s, 1, program_id);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)now);
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        *feedback = (MonitorFeedback)sqlite3_column_int(s, 0);
        *expires = (uint64_t)sqlite3_column_int64(s, 1);
    } else if (rc != SQLITE_DONE) monitor_store_set_error(e, "feedback read");
    sqlite3_finalize(s);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}


int monitor_store_trim(MonitorStore *e, uint64_t now)
{
    /* 참조하는 행을 고려한 순서로 삭제하며 단계마다 SQL의 삭제 개수 상한을 적용한다. */
    static const struct RetentionStep {
        const char *sql;
        uint64_t retention;
        int protect_current_session;
    } steps[] = {
        { "DELETE FROM raw WHERE id IN(SELECT id FROM raw WHERE utc<? ORDER BY utc LIMIT 2000)", 3600 * TICKS, 0 },
        { "DELETE FROM aggregate_window WHERE id IN(SELECT id FROM aggregate_window WHERE end_utc<? ORDER BY end_utc LIMIT 1000)", 28 * DAY, 0 },
        { "DELETE FROM collection_run WHERE id IN(SELECT id FROM collection_run WHERE finished<? AND NOT EXISTS(SELECT 1 FROM aggregate_window w WHERE w.run_id=collection_run.id) AND NOT EXISTS(SELECT 1 FROM raw r WHERE r.run_id=collection_run.id) ORDER BY finished LIMIT 1000)", 28 * DAY, 0 },
        { "DELETE FROM feedback WHERE id IN(SELECT id FROM feedback WHERE expires<? ORDER BY expires LIMIT 1000)", 7 * DAY, 0 },
        { "DELETE FROM session WHERE id IN(SELECT id FROM session WHERE id<>? AND started<? AND NOT EXISTS(SELECT 1 FROM collection_run r WHERE r.session_id=session.id) ORDER BY started LIMIT 1000)", 28 * DAY, 1 },
        { "DELETE FROM program WHERE id IN(SELECT id FROM program WHERE last_seen<? AND NOT EXISTS(SELECT 1 FROM raw r WHERE r.program_id=program.id) AND NOT EXISTS(SELECT 1 FROM aggregate_window w WHERE w.program_id=program.id) AND NOT EXISTS(SELECT 1 FROM feedback f WHERE f.program_id=program.id) LIMIT 128)", 28 * DAY, 0 }
    };
    unsigned i;
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        sqlite3_stmt *s = NULL;
        uint64_t retention = steps[i].retention;
        if (!monitor_store_prepare(e, steps[i].sql, &s)) return 0;
        if (steps[i].protect_current_session) {
            sqlite3_bind_int64(s, 1, e->session);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)(now > retention ? now - retention : 0));
        } else {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)(now > retention ? now - retention : 0));
        }
        if (!monitor_store_done(e, s)) {
            sqlite3_finalize(s);
            return 0;
        }
        sqlite3_finalize(s);
    }
    return 1;
}


int monitor_store_record_failure(MonitorStore *e, uint64_t utc, unsigned status)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!e || utc > INT64_MAX) return 0;
    if (!monitor_store_prepare(e, "INSERT INTO collection_run(session_id,started,finished,status,row_count,cpu_count) VALUES(?,?,?,?,0,0)", &s)) return 0;
    sqlite3_bind_int64(s, 1, e->session);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)utc);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)utc);
    sqlite3_bind_int64(s, 4, status ? status : 1);
    ok = monitor_store_done(e, s);
    sqlite3_finalize(s);
    return ok;
}


int monitor_store_feedback(MonitorStore *e, int64_t program, MonitorFeedback kind, uint64_t now)
{
    sqlite3_stmt *s = NULL;
    uint64_t duration = kind == MONITOR_FEEDBACK_IGNORE ? 3600 * TICKS : 86400 * TICKS;
    if (!e || program <= 0 || kind < MONITOR_FEEDBACK_CANCEL || kind > MONITOR_FEEDBACK_EXPECTED || now > INT64_MAX - duration) return 0;
    if (!monitor_store_execute(e, "BEGIN IMMEDIATE")) return 0;
    if (!monitor_store_prepare(e, "UPDATE feedback SET cancelled=1 WHERE program_id=? AND cancelled=0", &s)) goto failed;
    sqlite3_bind_int64(s, 1, program);
    if (!monitor_store_done(e, s)) goto failed;
    sqlite3_finalize(s);
    s = NULL;
    if (!monitor_store_prepare(e, "INSERT INTO feedback(program_id,kind,created,expires) VALUES(?,?,?,?)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, program);
    sqlite3_bind_int(s, 2, kind);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)(kind ? now + duration : now));
    if (!monitor_store_done(e, s)) goto failed;
    sqlite3_finalize(s);
    s = NULL;
    if (!monitor_store_trim(e, now) || !monitor_store_execute(e, "COMMIT")) goto failed;
    return 1;
failed:
    sqlite3_finalize(s);
    sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    return 0;
}


const char *monitor_store_error(const MonitorStore *e) { return e ? e->error : "engine unavailable"; }
int64_t monitor_store_session(const MonitorStore *e) { return e->session; }
int monitor_store_begin(MonitorStore *e, int64_t *cutoff)
{
    int ok;
    if (!monitor_store_execute(e, "BEGIN IMMEDIATE")) return 0;
    /* 같은 회차에서 먼저 저장한 창이 다른 인스턴스의 비교 이력에 들어가지 않게 한다. */
    *cutoff = monitor_store_scalar(e, "SELECT COALESCE(max(id),0) FROM aggregate_window", &ok);
    return ok;
}
int monitor_store_commit(MonitorStore *e) { return monitor_store_execute(e, "COMMIT"); }
void monitor_store_rollback(MonitorStore *e) { sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL); }


int monitor_store_insert_run(MonitorStore *e, const ResourceBatch *batch, int64_t *run)
{
    sqlite3_stmt *s = NULL;
    if (!monitor_store_prepare(e, "INSERT INTO collection_run(session_id,started,finished,status,row_count,cpu_count,foreground_utc,foreground_pid) VALUES(?,?,?,0,?,?,?,?)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, e->session);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)batch->started_at_utc_100ns);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)batch->finished_at_utc_100ns);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)batch->count);
    sqlite3_bind_int64(s, 5, batch->logical_processor_count);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)batch->foreground_observed_at_utc_100ns);
    if (batch->foreground_pid) sqlite3_bind_int64(s, 7, batch->foreground_pid);
    if (!monitor_store_done(e, s)) goto failed;
    sqlite3_finalize(s);
    s = NULL;
    *run = sqlite3_last_insert_rowid(e->db);
    return 1;
failed:
    sqlite3_finalize(s);
    return 0;
}


int monitor_store_touch_run_programs(MonitorStore *e, int64_t run, uint64_t now)
{
    sqlite3_stmt *s = NULL;
    if (!monitor_store_prepare(e, "UPDATE program SET last_seen=max(last_seen,?) WHERE id IN(SELECT program_id FROM raw WHERE run_id=? AND program_id IS NOT NULL)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)now);
    sqlite3_bind_int64(s, 2, run);
    if (!monitor_store_done(e, s)) goto failed;
    sqlite3_finalize(s);
    s = NULL;
    return 1;
failed:
    sqlite3_finalize(s);
    return 0;
}


static void bind_window_metric(sqlite3_stmt *statement, sqlite3_int64 window_id,
    unsigned metric, const MonitorMetricReport *report)
{
    sqlite3_bind_int64(statement, 1, window_id);
    sqlite3_bind_int(statement, 2, (int)metric);
    monitor_store_bind_value(statement, 3, isfinite(report->current), report->current);
    sqlite3_bind_double(statement, 4, report->covered_seconds);
    sqlite3_bind_int(statement, 5, (int)report->valid_samples);
    sqlite3_bind_int(statement, 6, report->baseline_eligible);
    sqlite3_bind_int(statement, 7, report->state);
    sqlite3_bind_text(statement, 8, report->reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 9, (int)report->count);
    sqlite3_bind_int(statement, 10, (int)report->days);
    sqlite3_bind_int(statement, 11, (int)report->minutes);
    monitor_store_bind_value(statement, 12, isfinite(report->median), report->median);
    monitor_store_bind_value(statement, 13, isfinite(report->mad), report->mad);
    monitor_store_bind_value(statement, 14, isfinite(report->threshold), report->threshold);
    sqlite3_bind_int(statement, 15, (int)report->streak);
}


int monitor_store_begin_window(MonitorStore *e, const MonitorWindowHeader *p, MonitorWindowWrite *scope)
{
    sqlite3_stmt *s = NULL;
    scope->statement = NULL;
    scope->window_id = 0;
    if (!monitor_store_prepare(e, "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,?,?,?,?,?,1)", &s)) return 0;
    sqlite3_bind_int64(s, 1, p->run_id);
    sqlite3_bind_int64(s, 2, p->program_id);
    sqlite3_bind_int64(s, 3, p->pid);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)p->creation);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)p->start);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)p->end);
    sqlite3_bind_double(s, 7, p->duration);
    if (!monitor_store_done(e, s)) {
        sqlite3_finalize(s);
        return 0;
    }
    sqlite3_finalize(s);
    s = NULL;
    scope->window_id = sqlite3_last_insert_rowid(e->db);
    if (!monitor_store_prepare(e, "INSERT INTO aggregate_metric VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &s)) {
        sqlite3_finalize(s);
        return 0;
    }
    scope->statement = s;
    return 1;
}


int monitor_store_write_metric(MonitorStore *e, MonitorWindowWrite *scope, unsigned metric, const MonitorMetricReport *report)
{
    sqlite3_stmt *s = scope->statement;
    bind_window_metric(s, scope->window_id, metric, report);
    return monitor_store_done(e, s);
}
void monitor_store_end_window(MonitorWindowWrite *scope)
{
    sqlite3_finalize(scope->statement);
    scope->statement = NULL;
}
