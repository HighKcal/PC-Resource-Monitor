#ifndef MONITOR_STORAGE_INTERNAL_H
#define MONITOR_STORAGE_INTERNAL_H
#include "monitor_internal.h"

/* Private worker-thread helpers; transaction scope belongs to engine/monitor.c. */
void monitor_db_set_error(MonitorEngine *e, const char *what);
int monitor_db_execute(MonitorEngine *e, const char *sql);
int monitor_db_prepare(MonitorEngine *e, const char *sql, sqlite3_stmt **s);
int monitor_db_done(MonitorEngine *e, sqlite3_stmt *s);
int64_t monitor_db_scalar(MonitorEngine *e, const char *sql, int *ok);
int64_t monitor_db_program_id(MonitorEngine *e, const char *path, const char *name, uint64_t utc);
void monitor_db_bind_value(sqlite3_stmt *s, int index, int valid, double value);
int monitor_db_save_raw(MonitorEngine *e, sqlite3_int64 run, const ResourceData *r, const MonitorInstance *p);
int monitor_db_get_feedback(MonitorEngine *e, MonitorRow *r, uint64_t now);
int monitor_db_trim(MonitorEngine *e, uint64_t now);
#endif
