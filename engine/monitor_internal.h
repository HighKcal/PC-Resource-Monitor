#ifndef MONITOR_INTERNAL_H
#define MONITOR_INTERNAL_H
#include "monitor.h"
#include "../third_party/sqlite/sqlite3.h"
typedef struct MonitorInstance MonitorInstance;
struct MonitorInstance {
    uint32_t pid, path_error;
    uint64_t creation, start, last_utc;
    int64_t program_id;
    char *path;
    double duration, sum[4], covered[4];
    unsigned samples[4], streak[4];
    MonitorMetricReport report[4];
};

struct MonitorEngine {
    sqlite3 *db;
    sqlite3_stmt *raw_insert, *program_find, *program_insert, *baseline_query;
    MonitorInstance *instances;
    size_t count;
    int synthetic;
    sqlite3_int64 session, sequence;
    char error[256];
};
#define MONITOR_TICKS_PER_SECOND UINT64_C(10000000)
#define MONITOR_DAY_TICKS (UINT64_C(86400)*MONITOR_TICKS_PER_SECOND)
#endif
