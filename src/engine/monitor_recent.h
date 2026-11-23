#ifndef MONITOR_RECENT_H
#define MONITOR_RECENT_H
#include "monitor.h"
#include "../detection/monitor_policy.h"
/* 선택 상세의 설명용 근거다. 학습·연속 초과·후보·알림 정책의 입력으로 사용하지 않는다.
 * 단위는 MonitorMetricReport와 같고 current는 현재 표본이 아닌 최신 유효 완료창의 평균이다. */
enum { MONITOR_RECENT_NO_WINDOW, MONITOR_RECENT_PREPARING,
    MONITOR_RECENT_READY, MONITOR_RECENT_LONG_TERM };
/* 기존 소비자 이름·값을 유지하되 비GUI 숫자 규칙은 monitor_policy.h에서 정한다. */
enum { MONITOR_RECENT_MIN_HISTORY = MONITOR_RECENT_READY_WINDOWS,
    MONITOR_RECENT_MAX_HISTORY = MONITOR_RECENT_HISTORY_LIMIT };
typedef struct MonitorRecentMetric {
    uint32_t state, count;
    int64_t anchor_window_id;
    double current, median, window_duration_seconds;
    uint64_t window_start_utc_100ns, window_end_utc_100ns;
    uint64_t history_start_utc_100ns, history_end_utc_100ns;
} MonitorRecentMetric;
typedef struct MonitorRecentComparison { MonitorRecentMetric metric[MONITOR_METRICS]; } MonitorRecentComparison;
/* 직렬 worker 전용 내부 API. 호출자는 최신 snapshot의 행을 전달하며 현재 engine 인스턴스의
 * 신원·경로와 대조한다. 한 지표가 잠시 결측이어도 이전 유효 완료창의 anchor는 유지한다.
 * 성공 결과는 값 사본으로 호출자가 소유하고 별도 해제가 필요 없다. 실패하면 출력은 비어 있다.
 * ingest나 일반 snapshot/목록 생성은 이 이력 조회를 실행하지 않는다. */
int monitor_recent_read(MonitorEngine *, const MonitorRow *, MonitorRecentComparison *);
#endif
