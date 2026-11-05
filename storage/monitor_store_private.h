#ifndef MONITOR_STORE_PRIVATE_H
#define MONITOR_STORE_PRIVATE_H
#include "monitor_store.h"
#include "../third_party/sqlite/sqlite3.h"
#include "monitor_policy.h"
/* storage 구현 TU만 공유하는 DB·prepared statement 배치와 SQL 보조 함수다. */
struct MonitorStore {
    sqlite3 *db;
    sqlite3_stmt *raw_insert, *program_find, *program_insert, *baseline_query;
    int64_t session;
    char error[256];
};
void monitor_store_set_error(MonitorStore *, const char *);
int monitor_store_execute(MonitorStore *, const char *);
int monitor_store_prepare(MonitorStore *, const char *, sqlite3_stmt **);
int monitor_store_done(MonitorStore *, sqlite3_stmt *);
int64_t monitor_store_scalar(MonitorStore *, const char *, int *);
void monitor_store_bind_value(sqlite3_stmt *, int, int, double);
int monitor_history_prepare(MonitorStore *store);
#endif
