#ifndef MONITOR_COMPLETED_H
#define MONITOR_COMPLETED_H
#include "monitor.h"
typedef struct MonitorCompletedMetric {
    double average, median, upper;
} MonitorCompletedMetric;
/* 마지막 완료창의 표시 근거만 담은 값 사본이다. state는 metric*4 위치의
 * 0(완료창 없음) 또는 MonitorState+1이다. 창 시각은 FILETIME UTC 100ns,
 * duration은 QPC 관측 초, 지표 수치 단위는 MonitorMetricReport와 같다.
 * 현재 표본·연속 판정과 독립적이며 후보/알림 입력으로 사용하지 않는다. */
typedef struct MonitorCompletedWindow {
    uint64_t start, end;
    double duration;
    uint32_t states;
    MonitorCompletedMetric metric[MONITOR_METRICS];
} MonitorCompletedWindow;
/* 최신 snapshot의 행 순서대로 복사하는 읽기 전용 내부 경계다. 호출자가 count개
 * 출력 공간을 소유하며 SQL·할당·오류 상태 변경은 없다. 같은 engine의 최신 회차만
 * 성공한다. 출력은 실패 시 사용하지 않으며 별도 해제가 필요 없다. */
int monitor_completed_windows(MonitorEngine *, const MonitorSnapshot *, MonitorCompletedWindow *out, size_t count);
#endif
