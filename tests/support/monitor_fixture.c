#include "monitor_fixture.h"
/* 과거 기준 소스와의 호환 처리는 이 시험 어댑터에만 둔다. */
#ifdef MONITOR_TYPES_H
#include "monitor_engine_private.h"
#include "monitor_store_private.h"
sqlite3 *monitor_fixture_db(MonitorEngine *e) { return e->store->db; }
sqlite3_stmt *monitor_fixture_baseline_statement(MonitorEngine *e) { return e->store->baseline_query; }
MonitorStore *monitor_fixture_store(MonitorEngine *e) { return e->store; }
void monitor_fixture_set_commit_epoch(MonitorEngine *e, uint64_t epoch) { e->mutation_epoch = epoch; }
uint64_t monitor_fixture_instance_start(const MonitorEngine *e, size_t i) { return e->instances[i].window.start; }
#else
#include "monitor_internal.h"
sqlite3 *monitor_fixture_db(MonitorEngine *e) { return e->db; }
sqlite3_stmt *monitor_fixture_baseline_statement(MonitorEngine *e) { return e->baseline_query; }
MonitorStore *monitor_fixture_store(MonitorEngine *e) { (void)e; return NULL; }
void monitor_fixture_set_commit_epoch(MonitorEngine *e, uint64_t epoch) { (void)e; (void)epoch; }
uint64_t monitor_fixture_instance_start(const MonitorEngine *e, size_t i) { return e->instances[i].start; }
#endif
size_t monitor_fixture_instance_count(const MonitorEngine *e) { return e->count; }
int monitor_fixture_instances_empty(const MonitorEngine *e) { return e->instances == NULL; }
int monitor_fixture_cache_empty(const MonitorEngine *e) {
#ifdef MONITOR_TYPES_H
    return e->history.cache == NULL;
#else
    return e->baseline_cache == NULL;
#endif
}
int64_t monitor_fixture_instance_program(const MonitorEngine *e, size_t i) { return e->instances[i].program_id; }
