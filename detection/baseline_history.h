#ifndef BASELINE_HISTORY_H
#define BASELINE_HISTORY_H
#include "monitor_history.h"
typedef struct BaselineHistoryCache BaselineHistoryCache;
typedef struct BaselineHistoryScope {
    MonitorStore *store;
    int64_t cutoff;
    BaselineHistoryCache *cache;
    int cache_enabled;
} BaselineHistoryScope;
/* 최초 begin 전에 0 초기화한다. 호출자의 트랜잭션 안에서만 store를 빌리며,
 * trim·rollback·close·다음 begin 전에 clear한다. 캐시 결과는 이 범위에만 유효하다.
 * 지연 할당 실패 시 회차 내 재시도 없이 SQL을 사용한다. */
void baseline_history_begin(BaselineHistoryScope *scope, MonitorStore *store, int64_t cutoff);
void baseline_history_clear(BaselineHistoryScope *scope);
int baseline_history_select_day(BaselineHistoryScope *scope, int64_t program_id, unsigned metric,
    uint64_t lower, uint64_t upper, uint64_t exact_start,
    double values[BASELINE_SAMPLES_PER_DAY], unsigned *count);
#endif
