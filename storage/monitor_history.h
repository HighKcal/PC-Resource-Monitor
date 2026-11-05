#ifndef MONITOR_HISTORY_H
#define MONITOR_HISTORY_H
#include "monitor_store.h"
#include "monitor_policy.h"
typedef struct MonitorHistoryDayRequest {
    int64_t program_id, cutoff;
    uint64_t lower, upper, exact_start;
    unsigned metric;
} MonitorHistoryDayRequest;
/* 호출자가 트랜잭션을 소유하고 새 창을 저장하기 전에 cutoff를 고정한다.
 * 날짜 상한과 정밀한 창 시작시각은 각각 적용한다. 완전한 결과만 복사하며,
 * 실패 시 statement를 reset·clear하고 count는 0, values는 기존 값으로 유지한다. */
int monitor_history_select_day(MonitorStore *store, const MonitorHistoryDayRequest *request,
    double values[BASELINE_SAMPLES_PER_DAY], unsigned *count);
#endif
