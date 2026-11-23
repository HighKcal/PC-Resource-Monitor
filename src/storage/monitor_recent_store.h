#ifndef MONITOR_RECENT_STORE_H
#define MONITOR_RECENT_STORE_H
#include "monitor_store.h"
#include "../engine/monitor_recent.h"
typedef struct MonitorRecentRequest {
    int64_t session_id, program_id, anchor_window_id, first_run_id;
    uint32_t pid, metric;
    uint64_t creation, as_of_utc_100ns;
    const char *path;
} MonitorRecentRequest;
/* anchor=0이면 같은 세션·신원·대소문자까지 같은 경로에서 run_id DESC,id DESC로 최신 유효
 * 완료창을 찾는다. UTC 역행과 독립적인 저장 성공 순서이며 생성/관측 시각·reset 뒤 첫 회차로
 * 범위를 제한한다. anchor가 있으면 그 ID를 검증한다. 과거는 anchor보다 먼저 저장된 회차의
 * 종료 시각이 [anchor.start-60분,anchor.start]인 창에서 종료 분별 최신 대표 최대 30개다.
 * 이 범위·최소 과거 수·상한의 숫자 규칙은 monitor_policy.h에 있다. 최신창 자체는 수에 넣지 않는다.
 * 모든 조회가 성공한 값 사본만 반환한다. 실패 시 출력은 비며 statement·할당의 소유권은 넘기지 않는다. */
int monitor_store_recent_metric(MonitorStore *, const MonitorRecentRequest *, MonitorRecentMetric *);
#endif
