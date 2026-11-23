/* 한 회차의 store·cutoff·선택 버전에 묶인 날짜별 이력 캐시.
 * 조회와 통계 정책은 바꾸지 않고 성공한 선택값 사본만 재사용한다. */
#include <stdlib.h>
#include <string.h>
#include "baseline_history.h"

typedef struct BaselineDay {
    int64_t program;
    uint64_t lower, upper;
    unsigned metric, count;
    double values[BASELINE_SAMPLES_PER_DAY];
} BaselineDay;
struct BaselineHistoryCache {
    MonitorStore *store;
    int64_t cutoff;
    unsigned version, count;
    unsigned slots[BASELINE_CACHE_SLOTS]; /* 항목 인덱스 + 1. 0은 빈 슬롯. */
    BaselineDay entries[BASELINE_CACHE_ENTRIES];
};

void baseline_history_clear(BaselineHistoryScope *scope)
{
    if (!scope) return;
    free(scope->cache);
    scope->cache = NULL;
    scope->cache_enabled = 0;
    scope->store = NULL;
    scope->cutoff = 0;
}
void baseline_history_begin(BaselineHistoryScope *scope, MonitorStore *store, int64_t cutoff)
{
    baseline_history_clear(scope);
    scope->store = store;
    scope->cutoff = cutoff;
    scope->cache_enabled = 1;
}

static BaselineHistoryCache *baseline_cache(BaselineHistoryScope *scope, int64_t cutoff)
{
    BaselineHistoryCache *cache;
    if (!scope->cache_enabled) return NULL;
    cache = scope->cache;
    if (!cache) {
        cache = calloc(1, sizeof(*cache));
        /* 선택적 캐시의 할당 실패로 수집 저장을 실패시키지 않는다.
         * 이 회차는 SQL로 처리하고 날짜마다 할당을 재시도하지 않는다. */
        if (!cache) {
            scope->cache_enabled = 0;
            return NULL;
        }
        cache->store = scope->store;
        cache->cutoff = cutoff;
        cache->version = BASELINE_SELECTION_VERSION;
        scope->cache = cache;
    }
    if (cache->store != scope->store || cache->cutoff != cutoff || cache->version != BASELINE_SELECTION_VERSION) return NULL;
    return cache;
}


static unsigned baseline_cache_slot(const BaselineHistoryCache *cache, int64_t program,
    unsigned metric, uint64_t lower, uint64_t upper)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned slot;
    hash = (hash ^ (uint64_t)program) * UINT64_C(1099511628211);
    hash = (hash ^ metric) * UINT64_C(1099511628211);
    hash = (hash ^ lower) * UINT64_C(1099511628211);
    hash = (hash ^ upper) * UINT64_C(1099511628211);
    /* 슬롯 수가 2의 거듭제곱이므로 시각의 상위 비트도 하위 해시에 섞는다. */
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    slot = (unsigned)hash & (BASELINE_CACHE_SLOTS - 1);
    while (cache->slots[slot]) {
        const BaselineDay *day = &cache->entries[cache->slots[slot] - 1];
        if (day->program == program && day->metric == metric && day->lower == lower && day->upper == upper) break;
        slot = (slot + 1) & (BASELINE_CACHE_SLOTS - 1);
    }
    return slot;
}


int baseline_history_select_day(BaselineHistoryScope *scope, int64_t program, unsigned metric,
    uint64_t lower, uint64_t upper, uint64_t start,
    double values[BASELINE_SAMPLES_PER_DAY], unsigned *count)
{
    BaselineHistoryCache *cache;
    MonitorHistoryDayRequest request;
    if (count) *count = 0;
    if (!scope || !scope->store || !values || !count) return 0;
    cache = baseline_cache(scope, scope->cutoff);
    uint64_t effective_upper = upper < start ? upper : start;
    unsigned slot = cache ? baseline_cache_slot(cache, program, metric, lower, effective_upper) : 0;
    unsigned n = 0;
    if (cache && cache->slots[slot]) {
        const BaselineDay *day = &cache->entries[cache->slots[slot] - 1];
        *count = day->count;
        memcpy(values, day->values, day->count * sizeof(*values));
        return 1;
    }
    /* SQL은 정밀한 창 시작시각과 트랜잭션 cutoff 조건을 각각 적용한다.
     * 과거 날짜는 날짜 상한이 창 시작보다 앞서므로 같은 캐시 키를 공유할 수 있다. */
    request.program_id = program; request.metric = metric;
    request.lower = lower; request.upper = upper; request.exact_start = start;
    request.cutoff = scope->cutoff;
    if (!monitor_history_select_day(scope->store, &request, values, &n)) return 0;
    /* 조회가 모두 성공한 결과만 캐시에 넣는다. 용량이 차면 기존 항목을 유지하고
     * 새 키는 SQL로 처리한다. */
    if (cache && cache->count < BASELINE_CACHE_ENTRIES) {
        BaselineDay *day = &cache->entries[cache->count];
        day->program = program;
        day->metric = metric;
        day->lower = lower;
        day->upper = effective_upper;
        day->count = n;
        memcpy(day->values, values, n * sizeof(*values));
        cache->slots[slot] = ++cache->count;
    }
    *count = n;
    return 1;
}

