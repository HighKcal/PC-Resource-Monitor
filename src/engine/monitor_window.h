#ifndef MONITOR_WINDOW_H
#define MONITOR_WINDOW_H
#include "monitor_types.h"
/* 인스턴스에 포함되는 누적 값. DB·문자열·힙 자원을 소유하지 않는다. */
typedef struct MonitorWindowAccumulator {
    uint64_t start, last_utc;
    double duration, sum[MONITOR_METRICS], covered[MONITOR_METRICS];
    unsigned samples[MONITOR_METRICS];
} MonitorWindowAccumulator;
typedef struct MonitorWindowStep { int reset, complete; unsigned missing; } MonitorWindowStep;
void monitor_window_unavailable(MonitorMetricReport *report, const char *reason);
void monitor_window_reset(MonitorWindowAccumulator *window, uint64_t anchor);
/* raw를 빌려 산술 누적만 수행한다. reset이면 호출자가 raw UTC에서 창을 재시작한다.
 * missing은 metric 인덱스 비트이며, 호출자가 연속 초과 상태와 보고서에 반영한다. */
MonitorWindowStep monitor_window_observe(MonitorWindowAccumulator *window, const ResourceData *raw, int program_known);
/* 호출자의 report에 품질·현재값·관측창 정보를 채우고 Baseline 비교 가능 여부를 반환한다.
 * 연속 초과 상태 반영과 metric별 순차 저장은 호출자가 담당한다. */
int monitor_window_metric(const MonitorWindowAccumulator *window, unsigned metric, MonitorMetricReport *report);
#endif
