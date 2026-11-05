#ifndef MONITOR_BASELINE_H
#define MONITOR_BASELINE_H
#include "baseline_history.h"
typedef struct BaselineInput {
    int64_t program_id;
    uint64_t exact_start;
    unsigned metric, prior_streak;
} BaselineInput;
/* report에는 품질 검사를 통과한 유한한 현재값과 관측창 메타데이터가 들어온다.
 * 회차 cutoff와 현재 창 이전 조건을 만족하는 이력만 골라 report와 다음 연속 초과
 * 횟수를 채운다. engine·인스턴스·SQLite 내부 구조나 지속 상태를 소유하지 않는다. */
int monitor_baseline(BaselineHistoryScope *history, const BaselineInput *input,
    MonitorMetricReport *report, unsigned *next_streak);
#endif
