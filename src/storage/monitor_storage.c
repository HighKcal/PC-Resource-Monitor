/* 로컬 SQLite DB의 개방·검사와 원시/집계/피드백 저장, 보관 정리를 담당한다.
 * 개방은 monitor_store_open, 독립 쓰기는 monitor_store_commit_feedback/monitor_store_trim에서 읽는다.
 * 회차의 begin→write→commit은 engine이 조율한다. 피드백과 보관 정리는 각각 자기 트랜잭션을
 * 소유하며, 정리 실패는 이미 커밋한 회차·피드백을 되돌리지 않는다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <wchar.h>
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


/* 쓰기 statement를 한 번 실행한 뒤 성공·실패 모두 reset/clear한다. finalize는
 * 소유자가 맡으므로 재사용 statement와 임시 statement에 공통으로 쓸 수 있다. */
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


/* worker 진단용 읽기다. 실패 시 앞서 읽은 일부 필드가 남을 수 있으므로 반환값 1일 때만
 * 전체 통계를 사용한다. 원시 행 수 0은 성공한 조회에서만 빈 이력이라는 뜻이다. */
int monitor_store_stats(MonitorStore *e, MonitorStorageStats *out)
{
    int ok;
    if (!e || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (!monitor_store_recover_pending(e)) return 0;
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
    wchar_t *absolute = NULL;
    wchar_t full[32768];
    DWORD length;
    if (wide && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) {
        free(wide);
        wide = NULL;
    }
    if (!wide) return NULL;
    /* CWD는 이 한 호출에서만 해석한다. 길이 조회와 실제 변환 사이의 CWD 변경을 피한다. */
    length = GetFullPathNameW(wide, 32768, full, NULL);
    if (length && length < 32768) {
        absolute = malloc(((size_t)length + 1) * sizeof(wchar_t));
        if (absolute) memcpy(absolute, full, ((size_t)length + 1) * sizeof(wchar_t));
    }
    free(wide);
    return absolute;
}


static int check_local_path(const wchar_t *path)
{
    wchar_t root[4];
    if (!path[0] || path[0] == L'\\' || path[1] != L':') return 0;
    root[0] = path[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = 0;
    return GetDriveTypeW(root) != DRIVE_REMOTE;
}


/* 최초 스키마 작성 또는 기존 형식·합성 여부를 검사한다. 임시 statement는 이 함수가
 * 해제한다. 생성 중 실패해 열린 트랜잭션은 개방 실패 경로의 close가 rollback한다. */
static int open_schema(MonitorStore *e, int exists, int synthetic)
{
    sqlite3_stmt *statement = NULL;
    int ok = 0;
    int64_t version, appid;
    version = monitor_store_scalar(e, "PRAGMA user_version", &ok);
    if (!ok) return 0;
    appid = monitor_store_scalar(e, "PRAGMA application_id", &ok);
    if (!ok) return 0;
    if (exists && (version != 1 || appid != APP_ID)) {
        if (!version && !appid) {
            snprintf(e->error, sizeof(e->error), "Empty/unsupported DB preserved. Stop all monitor instances, back up and rename this DB, then retry with an unused DB path. Preserve matching WAL/SHM files.");
        } else {
            snprintf(e->error, sizeof(e->error), "Unknown or unsupported DB schema (app=%lld version=%lld)",
                (long long)appid, (long long)version);
        }
        return 0;
    }
    if (!exists) {
        if (!monitor_store_execute(e, "BEGIN IMMEDIATE") || !monitor_store_execute(e, monitor_schema)) return 0;
        if (!monitor_store_prepare(e, "INSERT INTO metadata VALUES(?)", &statement)) goto failed;
        sqlite3_bind_int(statement, 1, synthetic);
        if (!monitor_store_done(e, statement)) goto failed;
        sqlite3_finalize(statement);
        statement = NULL;
        if (!monitor_store_execute(e, "COMMIT")) return 0;
    }
    if (monitor_store_scalar(e, "SELECT synthetic FROM metadata", &ok) != synthetic || !ok) {
        snprintf(e->error, sizeof(e->error), "Real/SYNTHETIC DB mismatch or invalid metadata");
        return 0;
    }
    return 1;
failed:
    sqlite3_finalize(statement);
    return 0;
}


/* 완성된 DB만 no-replace rename으로 게시한다. 실패 시 지우는 대상은 CREATE_NEW로
 * 독점 생성한 staging의 열린 handle뿐이며 target 경로/크기로 소유권을 추정하지 않는다. */
static int create_database(MonitorStore *e, const wchar_t *target, int synthetic)
{
    static volatile LONG sequence;
    unsigned char *image = NULL;
    sqlite3_int64 bytes = 0, offset = 0;
    wchar_t *staging = NULL;
    FILE_RENAME_INFO *rename = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD failure = ERROR_SUCCESS;
    const wchar_t *basename = wcsrchr(target, L'\\');
    size_t target_length = wcslen(target), directory_length, basename_length, rename_size;
    DWORD staging_seed = GetTickCount() ^ GetCurrentProcessId();
    unsigned attempt;
    int published = 0, ok = 0;
    if (sqlite3_open_v2(":memory:", &e->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        monitor_store_set_error(e, "initialize DB");
        return 0;
    }
    if (!open_schema(e, 0, synthetic)) return 0;
    image = sqlite3_serialize(e->db, "main", &bytes, 0);
    if (!image || bytes <= 0) {
        snprintf(e->error, sizeof(e->error), "Cannot serialize initialized DB (out of memory)");
        goto finished;
    }
    if (sqlite3_close(e->db) != SQLITE_OK) {
        monitor_store_set_error(e, "close initialized DB");
        goto finished;
    }
    e->db = NULL;
    if (!basename) {
        snprintf(e->error, sizeof(e->error), "DB path has no parent directory");
        goto finished;
    }
    ++basename;
    directory_length = (size_t)(basename - target);
    basename_length = target_length - directory_length;
    /* Win32는 끝의 점/공백을 정규화한다. 그 alias도 staging 후보에서 제외한다. */
    while (basename_length && (basename[basename_length - 1] == L'.' || basename[basename_length - 1] == L' ')) --basename_length;
    if (directory_length + 13 >= 32768) {
        snprintf(e->error, sizeof(e->error), "DB path is too long for safe initialization");
        goto finished;
    }
    staging = malloc((directory_length + 13) * sizeof(wchar_t));
    rename_size = offsetof(FILE_RENAME_INFO, FileName) + (target_length + 1) * sizeof(wchar_t);
    rename = malloc(rename_size);
    if (!staging || !rename) {
        snprintf(e->error, sizeof(e->error), "Out of memory initializing DB");
        goto finished;
    }
    memset(rename, 0, rename_size);
    rename->ReplaceIfExists = FALSE;
    rename->FileNameLength = (DWORD)(target_length * sizeof(wchar_t));
    memcpy(rename->FileName, target, (target_length + 1) * sizeof(wchar_t));
    memcpy(staging, target, directory_length * sizeof(wchar_t));
    for (attempt = 0; attempt < 16; ++attempt) {
        DWORD suffix = (staging_seed + (DWORD)InterlockedIncrement(&sequence)) & 0x0fffffff;
        /* 8.3 이름 자체를 써서 생성 시 별도 short-name alias가 target이 되는 것도 피한다. */
        swprintf(staging + directory_length, 13, L"m%07lx.tmp", (unsigned long)suffix);
        if (basename_length == 12 && _wcsnicmp(basename, staging + directory_length, 12) == 0) continue;
        file = CreateFileW(staging, GENERIC_WRITE | DELETE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        failure = GetLastError();
        if (failure != ERROR_FILE_EXISTS && failure != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) goto file_failed;
    while (offset < bytes) {
        sqlite3_int64 remaining = bytes - offset;
        DWORD count = (DWORD)(remaining > 1048576 ? 1048576 : remaining), written = 0;
        if (!WriteFile(file, image + (size_t)offset, count, &written, NULL) || written != count) {
            failure = GetLastError();
            if (!failure) failure = ERROR_WRITE_FAULT;
            goto file_failed;
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) {
        failure = GetLastError();
        goto file_failed;
    }
    if (!SetFileInformationByHandle(file, FileRenameInfo, rename, (DWORD)rename_size)) {
        failure = GetLastError();
        /* 다른 생성자가 먼저 게시했으면 그 파일을 CREATE 없이 열어 검사한다. */
        if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES) {
            ok = 1;
            goto finished;
        }
        goto file_failed;
    }
    published = 1;
    ok = 1;
    goto finished;
file_failed:
    snprintf(e->error, sizeof(e->error), "Cannot publish initialized DB (Windows error %lu)",
        (unsigned long)failure);
finished:
    if (file != INVALID_HANDLE_VALUE) {
        if (!published) {
            FILE_DISPOSITION_INFO disposition;
            disposition.DeleteFile = TRUE;
            /* 정리 실패도 target에는 영향을 주지 않으며 다음 초기화를 막지 않는다. */
            SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        CloseHandle(file);
    }
    free(rename);
    free(staging);
    sqlite3_free(image);
    return ok;
}


/* 연결 설정이 모두 확인되기 전에는 session 행을 만들지 않는다. WAL의 결과 문자열은
 * statement에 빌린 값이므로 비교를 끝낸 뒤 해제한다. 실패해도 이 함수의 statement는 남지 않는다. */
static int configure_connection(MonitorStore *e)
{
    sqlite3_stmt *statement = NULL;
    int wal, ok;
    const unsigned char *mode;
    if (!monitor_store_execute(e, "PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA main.cache_size=-16384")) return 0;
    if (!monitor_store_prepare(e, "PRAGMA journal_mode=WAL", &statement)) goto failed;
    if (sqlite3_step(statement) != SQLITE_ROW) {
        monitor_store_set_error(e, "enable WAL");
        goto failed;
    }
    mode = sqlite3_column_text(statement, 0);
    wal = mode && sqlite3_stricmp((const char *)mode, "wal") == 0;
    sqlite3_finalize(statement);
    statement = NULL;
    if (!wal) {
        snprintf(e->error, sizeof(e->error), "DB could not enable required WAL journal mode");
        return 0;
    }
    if (!monitor_store_execute(e, "PRAGMA synchronous=FULL")) return 0;
    if (monitor_store_scalar(e, "PRAGMA synchronous", &ok) != 2 || !ok ||
        monitor_store_scalar(e, "PRAGMA busy_timeout", &ok) != 250 || !ok) {
        snprintf(e->error, sizeof(e->error), "DB requires synchronous FULL and busy timeout 250ms");
        return 0;
    }
    return 1;
failed:
    sqlite3_finalize(statement);
    return 0;
}

/* session INSERT는 autocommit이다. 이후 공용 statement 준비가 실패하면 이 행은 남되
 * ready가 아직 0이므로 close가 정상 종료 시각을 붙이지 않는다. */
static int start_session(MonitorStore *e)
{
    sqlite3_stmt *statement = NULL;
    if (!monitor_store_prepare(e, "INSERT INTO session(started) VALUES(?)", &statement)) goto failed;
    sqlite3_bind_int64(statement, 1, (sqlite3_int64)monitor_utc_now());
    if (!monitor_store_done(e, statement)) goto failed;
    sqlite3_finalize(statement);
    e->session = sqlite3_last_insert_rowid(e->db);
    return 1;
failed:
    sqlite3_finalize(statement);
    return 0;
}


static int prepare_store_statements(MonitorStore *e)
{
    if (!monitor_store_prepare(e, "SELECT id FROM program WHERE path=?", &e->program_find) ||
        !monitor_store_prepare(e, "INSERT INTO program(path,name,first_seen,last_seen) VALUES(?,?,?,?)", &e->program_insert) ||
        !monitor_store_prepare(e, "INSERT INTO raw(run_id,utc,pid,creation,program_id,valid,cpu,ram,io_read,io_write,elapsed,observation_interval,cpu_interval,io_interval,foreground,cpu_status,read_status,write_status,open_error,times_error,memory_error,io_error,name_error,path_error,name) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &e->raw_insert) ||
        !monitor_history_prepare(e)) return 0;
    return 1;
}


/* out은 성공한 store만 받는다. 임시 파일의 게시, 실제 연결의 검사·설정, session과 공용
 * statement 준비를 순서대로 마친 뒤 ready를 세운다. 실패는 오류 사본을 먼저 남기고
 * close로 연결·공용 statement를 해제한다. 이미 게시된 완성 DB와 기존 파일은 지우지 않는다. */
int monitor_store_open(const char *path, int synthetic, MonitorStore **out, char *error, size_t size)
{
    MonitorStore *e = NULL;
    wchar_t *wide = NULL;
    char *absolute = NULL;
    int length;
    if (out) *out = NULL;
    if (error && size) error[0] = 0;
    if (!out || !path || !path[0] || (synthetic != 0 && synthetic != 1)) return 0;
    wide = wide_path(path);
    if (!wide || !check_local_path(wide)) {
        if (error && size) snprintf(error, size, "A local disk UTF-8 DB path is required");
        free(wide);
        return 0;
    }
    e = calloc(1, sizeof(*e));
    if (!e) {
        if (error && size) snprintf(error, size, "Out of memory opening store");
        free(wide);
        return 0;
    }
    length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    absolute = length > 0 ? malloc((size_t)length) : NULL;
    if (!absolute || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, absolute, length, NULL, NULL)) {
        snprintf(e->error, sizeof(e->error), "Cannot prepare absolute UTF-8 DB path");
        goto failed;
    }
    if (GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES && !create_database(e, wide, synthetic)) goto failed;
    /* 완성된 DB를 연다. 존재 확인 뒤의 삭제·교체가 빈 DB를 만들지 않도록 CREATE를 금지한다. */
    if (sqlite3_open_v2(absolute, &e->db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        monitor_store_set_error(e, "open");
        goto failed;
    }
    sqlite3_extended_result_codes(e->db, 1);
    if (sqlite3_busy_timeout(e->db, 250) != SQLITE_OK) {
        monitor_store_set_error(e, "busy timeout");
        goto failed;
    }

    if (!open_schema(e, 1, synthetic)) goto failed;
    if (!configure_connection(e)) goto failed;
    if (!start_session(e)) goto failed;
    if (!prepare_store_statements(e)) goto failed;

    e->ready = 1;
    free(absolute);
    free(wide);
    *out = e;
    return 1;
failed:
    if (error && size) snprintf(error, size, "%s", e->error);
    monitor_store_close(e);
    free(absolute);
    free(wide);
    return 0;
}


/* 정상 종료 표시는 마지막이다. 공용 statement를 해제하고 미완료 쓰기 rollback과
 * autocommit 복귀를 확인한 경우에만 ended를 기록한다. 실패한 개방도 여기서 해제하지만
 * ready가 아니면 이미 남은 session을 정상 종료로 바꾸지 않는다. */
void monitor_store_close(MonitorStore *e)
{
    sqlite3_stmt *s = NULL;
    int can_end;
    if (!e) return;
    sqlite3_finalize(e->raw_insert);
    sqlite3_finalize(e->program_find);
    sqlite3_finalize(e->program_insert);
    sqlite3_finalize(e->baseline_query);
    can_end = e->db && e->ready && e->session;
    if (e->db && !sqlite3_get_autocommit(e->db)) {
        int rolled_back = sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK;
        if (!rolled_back || !sqlite3_get_autocommit(e->db)) can_end = 0;
    }
    if (can_end && sqlite3_prepare_v2(e->db, "UPDATE session SET ended=? WHERE id=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, (sqlite3_int64)monitor_utc_now());
        sqlite3_bind_int64(s, 2, e->session);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_close(e->db);
    free(e);
}


/* 회차 트랜잭션 안에서 경로별 ID를 찾거나 만든다. 기존 경로는 last_seen만 전진시키며
 * 문자열은 SQLite가 복사한다. 0은 실패이고 부분 변경의 rollback은 engine이 맡는다. */
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


/* 유효성 비트와 필드별 오류를 함께 저장한다. 비유효 필드는 바인딩하지 않아 NULL로 남고,
 * 유효한 0은 그대로 기록된다. 다음 행에 값이 새지 않도록 done이 바인딩을 비운다. */
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


/* 호출자가 연 정리 트랜잭션 안에서만 실행한다. 삭제 순서·기한·개수는 이 표에서 읽고,
 * 처음 실패한 단계에서 멈춘다. 여기서는 commit/rollback하지 않고 임시 statement만 해제한다. */
static int trim_rows(MonitorStore *e, uint64_t now, int *aggregate_changed)
{
    /* 참조하는 행을 고려한 순서로 삭제하며 단계마다 SQL의 삭제 개수 상한을 적용한다. */
    static const struct RetentionStep {
        const char *sql;
        uint64_t retention;
        int protect_current_session;
        int changes_recent_history;
    } steps[] = {
        { "DELETE FROM raw WHERE id IN(SELECT id FROM raw WHERE utc<? ORDER BY utc LIMIT 2000)", 3600 * TICKS, 0, 0 },
        { "DELETE FROM aggregate_window WHERE id IN(SELECT id FROM aggregate_window WHERE end_utc<? ORDER BY end_utc LIMIT 1000)", 28 * DAY, 0, 1 },
        { "DELETE FROM collection_run WHERE id IN(SELECT id FROM collection_run WHERE finished<? AND NOT EXISTS(SELECT 1 FROM aggregate_window w WHERE w.run_id=collection_run.id) AND NOT EXISTS(SELECT 1 FROM raw r WHERE r.run_id=collection_run.id) ORDER BY finished LIMIT 1000)", 28 * DAY, 0, 0 },
        { "DELETE FROM feedback WHERE id IN(SELECT id FROM feedback WHERE expires<? ORDER BY expires LIMIT 1000)", 7 * DAY, 0, 0 },
        { "DELETE FROM session WHERE id IN(SELECT id FROM session WHERE id<>? AND started<? AND NOT EXISTS(SELECT 1 FROM collection_run r WHERE r.session_id=session.id) ORDER BY started LIMIT 1000)", 28 * DAY, 1, 0 },
        { "DELETE FROM program WHERE id IN(SELECT id FROM program WHERE last_seen<? AND NOT EXISTS(SELECT 1 FROM raw r WHERE r.program_id=program.id) AND NOT EXISTS(SELECT 1 FROM aggregate_window w WHERE w.program_id=program.id) AND NOT EXISTS(SELECT 1 FROM feedback f WHERE f.program_id=program.id) LIMIT 128)", 28 * DAY, 0, 0 }
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
        if (steps[i].changes_recent_history && sqlite3_changes(e->db) > 0) *aggregate_changed = 1;
        sqlite3_finalize(s);
    }
    return 1;
}

/* 회차·피드백 커밋 뒤 호출하는 별도 작업이다. 이 정리의 실패만 되돌리며, 이미 끝난
 * 저장은 남는다. rollback도 실패하면 pending으로 남겨 다음 독립 쓰기/보호된 읽기에서 복구한다.
 * require_idle 거부는 caller 소유 트랜잭션일 수 있어 여기서 추가 rollback하지 않는다. */
int monitor_store_trim_with_result(MonitorStore *e, uint64_t now, int *aggregate_changed)
{
    int changed = 0;
    if (aggregate_changed) *aggregate_changed = 0;
    if (!monitor_store_require_idle(e)) return 0;
    if (!monitor_store_execute(e, "BEGIN IMMEDIATE") ||
        !trim_rows(e, now, &changed) || !monitor_store_commit(e)) goto failed;
    /* 뒤 단계 또는 COMMIT 실패로 rollback된 삭제는 조회 상태에 영향을 주지 않는다. */
    if (aggregate_changed) *aggregate_changed = changed;
    return 1;
failed:
    monitor_store_rollback(e);
    return 0;
}
int monitor_store_trim(MonitorStore *e, uint64_t now)
{
    return monitor_store_trim_with_result(e, now, NULL);
}


int monitor_store_record_failure(MonitorStore *e, uint64_t utc, unsigned status)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!e || utc > INT64_MAX || !monitor_store_require_idle(e)) return 0;
    if (!monitor_store_prepare(e, "INSERT INTO collection_run(session_id,started,finished,status,row_count,cpu_count) VALUES(?,?,?,?,0,0)", &s)) return 0;
    sqlite3_bind_int64(s, 1, e->session);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)utc);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)utc);
    sqlite3_bind_int64(s, 4, status ? status : 1);
    ok = monitor_store_done(e, s);
    sqlite3_finalize(s);
    return ok;
}


/* 기존 억제 취소와 새 이력을 하나의 변경으로 쓴다. 트랜잭션은 caller가 소유하며,
 * 성공/실패 모두 임시 statement를 해제한다. 실패 시 앞선 UPDATE도 caller가 함께 되돌린다. */
static int write_feedback_change(MonitorStore *e, int64_t program, MonitorFeedback kind,
    uint64_t now, uint64_t expires)
{
    sqlite3_stmt *s = NULL;
    if (!monitor_store_prepare(e, "UPDATE feedback SET cancelled=1 WHERE program_id=? AND cancelled=0", &s)) goto failed;
    sqlite3_bind_int64(s, 1, program);
    if (!monitor_store_done(e, s)) goto failed;
    sqlite3_finalize(s);
    s = NULL;
    if (!monitor_store_prepare(e, "INSERT INTO feedback(program_id,kind,created,expires) VALUES(?,?,?,?)", &s)) goto failed;
    sqlite3_bind_int64(s, 1, program);
    sqlite3_bind_int(s, 2, kind);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)expires);
    if (!monitor_store_done(e, s)) goto failed;
    sqlite3_finalize(s);
    return 1;
failed:
    sqlite3_finalize(s);
    return 0;
}

/* 유효한 요청 하나의 UPDATE+INSERT+COMMIT을 소유한다. 보관 정리는 이 함수 밖에서
 * 성공 뒤에만 실행하므로, 정리 실패와 피드백 자체 실패의 반환값이 섞이지 않는다. */
int monitor_store_commit_feedback(MonitorStore *e, int64_t program, MonitorFeedback kind,
    uint64_t now, uint64_t expires)
{
    if (!monitor_store_require_idle(e)) return 0;
    if (!monitor_store_execute(e, "BEGIN IMMEDIATE") ||
        !write_feedback_change(e, program, kind, now, expires) ||
        !monitor_store_commit(e)) goto failed;
    return 1;
failed:
    monitor_store_rollback(e);
    return 0;
}


const char *monitor_store_error(const MonitorStore *e) { return e ? e->error : "engine unavailable"; }
int64_t monitor_store_session(const MonitorStore *e) { return e->session; }
int monitor_store_begin(MonitorStore *e, int64_t *cutoff)
{
    int ok;
    if (!monitor_store_require_idle(e)) return 0;
    if (!monitor_store_execute(e, "BEGIN IMMEDIATE")) {
        monitor_store_rollback(e);
        return 0;
    }
    /* 같은 회차에서 먼저 저장한 창이 다른 인스턴스의 비교 이력에 들어가지 않게 한다. */
    *cutoff = monitor_store_scalar(e, "SELECT COALESCE(max(id),0) FROM aggregate_window", &ok);
    if (!ok) monitor_store_rollback(e);
    return ok;
}
int monitor_store_commit(MonitorStore *e) { return monitor_store_execute(e, "COMMIT"); }
void monitor_store_rollback(MonitorStore *e)
{
    if (!e || !e->db) return;
    if (!sqlite3_get_autocommit(e->db)) sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    e->rollback_pending = !sqlite3_get_autocommit(e->db);
}
/* 독립 쓰기가 시작되기 전의 경계다. 이 저장소가 앞서 되돌리다 실패한 tx만 재시도한다.
 * 정상 진행 중인 호출자 tx는 종료하지 않고 거부하며, 복구 실패는 pending으로 남긴다. */
int monitor_store_require_idle(MonitorStore *e)
{
    if (!e || !e->db) return 0;
    if (sqlite3_get_autocommit(e->db)) {
        e->rollback_pending = 0;
        return 1;
    }
    if (e->rollback_pending) {
        int rolled_back = sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK;
        e->rollback_pending = !sqlite3_get_autocommit(e->db);
        if (rolled_back && !e->rollback_pending) return 1;
        monitor_store_set_error(e, "recover unfinished transaction");
    } else {
        snprintf(e->error, sizeof(e->error), "An existing DB transaction must finish before an independent write");
    }
    return 0;
}
int monitor_store_recover_pending(MonitorStore *e)
{
    char original_error[sizeof(e->error)];
    if (!e || !e->db) return 0;
    if (!e->rollback_pending) return 1;
    memcpy(original_error, e->error, sizeof(original_error));
    if (monitor_store_require_idle(e)) return 1;
    /* 읽기가 미완료 삭제 결과를 노출하거나 최초 정리 오류를 덮지 않게 한다. */
    if (original_error[0]) memcpy(e->error, original_error, sizeof(e->error));
    return 0;
}


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


/* 창 헤더를 넣고 지표 4개에 재사용할 statement를 scope에 넘긴다. 별도 COMMIT은 없다.
 * 헤더 삽입 후 준비가 실패해도 engine의 회차 rollback이 그 헤더까지 되돌린다. */
int monitor_store_begin_window(MonitorStore *e, const MonitorWindowHeader *p, MonitorWindowWrite *scope)
{
    sqlite3_stmt *s = NULL;
    scope->statement = NULL;
    scope->window_id = 0;
    if (!monitor_store_prepare(e, "INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES(?,?,?,?,?,?,?,"
        MONITOR_POLICY_TEXT(MONITOR_AGGREGATION_VERSION) ")", &s)) return 0;
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
