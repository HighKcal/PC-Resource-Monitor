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
    int ready; /* open 전체 성공 전에는 session을 정상 종료로 기록하지 않는다. */
    char error[256];
    /* rollback을 시도했지만 autocommit으로 돌아오지 못한 경우만 1이다. 다음 독립 쓰기나
     * 보호된 읽기가 재시도한다. 호출자가 정상 진행 중인 트랜잭션과 구별하는 표식이다. */
    int rollback_pending;
};
void monitor_store_set_error(MonitorStore *, const char *);
int monitor_store_execute(MonitorStore *, const char *);
int monitor_store_prepare(MonitorStore *, const char *, sqlite3_stmt **);
int monitor_store_done(MonitorStore *, sqlite3_stmt *);
int64_t monitor_store_scalar(MonitorStore *, const char *, int *);
void monitor_store_bind_value(sqlite3_stmt *, int, int, double);
int monitor_history_prepare(MonitorStore *store);
#endif
