#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../support/monitor_fixture.h"
#include "monitor_policy.h"
static unsigned checks, failures, store_hits, engine_allocations, engine_frees, outstanding;
static int deny_store;
static void *engine_owned[32];
#define CHECK(v) do { ++checks; if (!(v)) { ++failures; printf("FAIL %u %s\n", (unsigned)__LINE__, #v); } } while (0)
void *review_store_calloc(size_t n, size_t size)
{
    if (deny_store) { ++store_hits; return NULL; }
    return calloc(n, size);
}
void *review_engine_calloc(size_t n, size_t size)
{
    void *p = calloc(n, size);
    if (p) { if (outstanding == 32) exit(3); engine_owned[outstanding++] = p; ++engine_allocations; }
    return p;
}
void review_engine_free(void *p)
{
    unsigned i;
    for (i = 0; i < outstanding; ++i) if (engine_owned[i] == p) {
        engine_owned[i] = engine_owned[--outstanding]; ++engine_frees; break;
    }
    free(p);
}
static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t v = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s); return v;
}
int main(int argc, char **argv)
{
    char path[1024], error[256];
    MonitorEngine *e = (MonitorEngine *)(uintptr_t)1;
    sqlite3 *db = NULL;
    if (argc != 2) return 2;
    snprintf(path, sizeof(path), "%s/store-open-%lu.sqlite", argv[1], (unsigned long)GetCurrentProcessId());
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    deny_store = 1;
    CHECK(!monitor_open(path, 1, &e, error, sizeof(error)));
    CHECK(e == NULL); CHECK(store_hits == 1);
    CHECK(strstr(error, "Out of memory opening store") != NULL);
    CHECK(outstanding == 0 && engine_allocations == 1 && engine_frees == 1);
    CHECK(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES);
    printf("STORE_OPEN_OOM hits=%u out_null=1 engine_outstanding=%u db_created=0 error=%s\n", store_hits, outstanding, error);
    deny_store = 0;
    CHECK(monitor_open(path, 1, &e, error, sizeof(error))); CHECK(e != NULL);
    if (e) {
        const char *query = sqlite3_sql(monitor_fixture_baseline_statement(e));
        CHECK(MONITOR_AGGREGATION_VERSION == 1u && BASELINE_SELECTION_VERSION == 1u);
        CHECK(BASELINE_HISTORY_UTC_DAYS == 14u && BASELINE_SAMPLES_PER_DAY == 64u);
        CHECK(BASELINE_CACHE_ENTRIES == 4096u && BASELINE_CACHE_SLOTS == 8192u);
        CHECK(BASELINE_READY_WINDOWS == 60u && BASELINE_READY_UTC_DAYS == 3u && BASELINE_READY_MINUTES == 60u);
        CHECK(BASELINE_CANDIDATE_STREAK == 3u);
        CHECK(MONITOR_WINDOW_MIN_SECONDS == 60 && MONITOR_WINDOW_MAX_SECONDS == 75 && MONITOR_MAX_INTERVAL_SECONDS == 15);
        CHECK(MONITOR_RAM_MIN_SAMPLES == 15u && MONITOR_MIN_COVERAGE_RATIO == 0.75 && MONITOR_MAX_COVERAGE_RATIO == 1.25);
        CHECK(BASELINE_CPU_FLOOR == 5.0 && BASELINE_RAM_FLOOR == 134217728.0 && BASELINE_READ_FLOOR == 1048576.0 && BASELINE_WRITE_FLOOR == 1048576.0);
        CHECK(query && strstr(query, "x.version=1") && strstr(query, "WHERE p.n<64"));
        printf("POLICY_SQL version=1 day_cap=64 history_days=14 cache_entries=4096 constants_match=1\n");
        CHECK(scalar(monitor_fixture_db(e), "SELECT count(*) FROM session") == 1);
        CHECK(scalar(monitor_fixture_db(e), "SELECT count(*) FROM session WHERE ended IS NULL") == 1);
        monitor_close(e);
    }
    CHECK(outstanding == 0 && engine_allocations == 2 && engine_frees == 2);
    CHECK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM session") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM session WHERE ended IS NOT NULL") == 1);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    printf("STORE_OPEN_RETRY session_count=1 session_ended=1 engine_outstanding=%u checks=%u failures=%u\n", outstanding, checks, failures);
    return failures ? 1 : 0;
}
