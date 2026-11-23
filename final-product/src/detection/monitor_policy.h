#ifndef MONITOR_POLICY_H
#define MONITOR_POLICY_H
#include <stdint.h>
/* 비GUI 관측·장기 판정·최근 비교의 숫자 규칙을 찾는 시작점이다. 기존 버전 1 값을 모았으며
 * 시간·메모리·전송률 단위는 초·바이트·바이트/초다. SQL에 넣는 정수는 접미사 없는 십진수로
 * 정의한다. MONITOR_POLICY_TEXT가 같은 값을 SQL과 이유 문구에 넣어 별도 숫자를 두지 않는다. */
#define MONITOR_POLICY_TEXT_INNER(value) #value
#define MONITOR_POLICY_TEXT(value) MONITOR_POLICY_TEXT_INNER(value)
#define MONITOR_TICKS_PER_SECOND UINT64_C(10000000)
#define MONITOR_MINUTE_TICKS 600000000
#define MONITOR_DAY_TICKS (UINT64_C(86400) * MONITOR_TICKS_PER_SECOND)
/* 저장·조회가 공유하는 현재 집계 버전이다. 버전을 바꾸는 작업은 스키마·과거 기록·외부 소비자의
 * 호환 검토가 필요하며 이 값 하나를 고치는 리팩터링으로 취급하지 않는다. */
#define MONITOR_AGGREGATION_VERSION 1
/* 현재 창의 품질 조건. 미달 지표는 장기 판정에 넘기지 않고 연속 초과를 끊는다. */
#define MONITOR_WINDOW_MIN_SECONDS 60
#define MONITOR_WINDOW_MAX_SECONDS 75
#define MONITOR_MAX_INTERVAL_SECONDS 15
#define MONITOR_RAM_MIN_SAMPLES 15u
#define MONITOR_MIN_COVERAGE_RATIO 0.75
#define MONITOR_MAX_COVERAGE_RATIO 1.25
/* 장기 기준은 경로별 과거의 학습 가능 창을 사용한다. 현재 창은 비교를 마친 뒤에만 저장한다. */
#define BASELINE_HISTORY_UTC_DAYS 14u
#define BASELINE_SAMPLES_PER_DAY 64
#define BASELINE_MAX_SAMPLES (BASELINE_HISTORY_UTC_DAYS * BASELINE_SAMPLES_PER_DAY)
#define BASELINE_READY_WINDOWS 60
#define BASELINE_READY_UTC_DAYS 3
#define BASELINE_READY_MINUTES 60
#define BASELINE_CANDIDATE_STREAK 3u
#define BASELINE_CACHE_ENTRIES 4096u
#define BASELINE_CACHE_SLOTS 8192u
#define BASELINE_SELECTION_VERSION 1u
#define BASELINE_MAD_NORMAL_SCALE 1.4826
#define BASELINE_MAD_MARGIN_MULTIPLIER 3
#define BASELINE_MEDIAN_MARGIN_RATIO 0.5
#define BASELINE_CPU_FLOOR 5.0
#define BASELINE_RAM_FLOOR 134217728.0
#define BASELINE_READ_FLOOR 1048576.0
#define BASELINE_WRITE_FLOOR 1048576.0
/* 최근 비교는 같은 세션·경로·인스턴스의 설명 전용이다. 최신 완료창과 별개인 유효 과거 창을
 * 센다. 과거 4개면 준비 중이며 5개부터 비교를 표시한다. 장기 기준의 준비 조건과 다르다.
 * 학습 포함·연속 초과·알림 후보 판정에는 사용하지 않는다. GUI의 준비 문구는 별도 소비처다. */
#define MONITOR_RECENT_READY_WINDOWS 5
#define MONITOR_RECENT_HISTORY_LIMIT 30
#define MONITOR_RECENT_HISTORY_MINUTES 60
#define MONITOR_RECENT_HISTORY_TICKS (UINT64_C(1) * MONITOR_RECENT_HISTORY_MINUTES * MONITOR_MINUTE_TICKS)
#endif
