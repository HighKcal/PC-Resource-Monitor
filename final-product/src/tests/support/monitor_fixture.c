#include "monitor_fixture.h"
/* 이 전달본의 분리된 engine/store 내부 핸들만 시험에 빌려 준다. */
#include "monitor_engine_private.h"
#include "monitor_store_private.h"
sqlite3 *monitor_fixture_db(MonitorEngine *e) { return e->store->db; }
sqlite3_stmt *monitor_fixture_baseline_statement(MonitorEngine *e) { return e->store->baseline_query; }
MonitorStore *monitor_fixture_store(MonitorEngine *e) { return e->store; }
void monitor_fixture_set_commit_epoch(MonitorEngine *e, uint64_t epoch) { e->mutation_epoch = epoch; }
uint64_t monitor_fixture_instance_start(const MonitorEngine *e, size_t i) { return e->instances[i].window.start; }
size_t monitor_fixture_instance_count(const MonitorEngine *e) { return e->count; }
int monitor_fixture_instances_empty(const MonitorEngine *e) { return e->instances == NULL; }
int monitor_fixture_cache_empty(const MonitorEngine *e) { return e->history.cache == NULL; }
int64_t monitor_fixture_instance_program(const MonitorEngine *e, size_t i) { return e->instances[i].program_id; }
