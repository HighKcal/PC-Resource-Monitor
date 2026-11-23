#ifndef MONITOR_FIXTURE_H
#define MONITOR_FIXTURE_H
/* 시험 전용으로 내부 핸들을 빌린다. 기대값과 판정 기준은 각 시험 호출자에 둔다. */
#include "monitor.h"
#include "sqlite3.h"
typedef struct MonitorStore MonitorStore;
#ifndef MONITOR_TICKS_PER_SECOND
#define MONITOR_TICKS_PER_SECOND UINT64_C(10000000)
#define MONITOR_DAY_TICKS (UINT64_C(86400) * MONITOR_TICKS_PER_SECOND)
#endif
sqlite3 *monitor_fixture_db(MonitorEngine *);
sqlite3_stmt *monitor_fixture_baseline_statement(MonitorEngine *);
MonitorStore *monitor_fixture_store(MonitorEngine *);
size_t monitor_fixture_instance_count(const MonitorEngine *);
int monitor_fixture_instances_empty(const MonitorEngine *);
int monitor_fixture_cache_empty(const MonitorEngine *);
uint64_t monitor_fixture_instance_start(const MonitorEngine *, size_t);
int64_t monitor_fixture_instance_program(const MonitorEngine *, size_t);
/* 참조 발급 전 새 engine에서만 사용하는 epoch 소진 시험 경계. */
void monitor_fixture_set_commit_epoch(MonitorEngine *engine, uint64_t epoch);
#endif
