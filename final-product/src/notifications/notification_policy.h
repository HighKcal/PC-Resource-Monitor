#ifndef MONITOR_NOTIFICATION_POLICY_H
#define MONITOR_NOTIFICATION_POLICY_H

#include "monitor_types.h"

/* Shell과 분리된 알림 정책이다. observe → begin → dispatch → result 순서로 사용한다.
 * now_ms는 GetTickCount64 계열의 단조 밀리초, now_utc/expires와 창 시각은 UTC 100ns다.
 * 대기·빈도·재시도 상수는 아래, 재발의 증가 기준은 notification_policy.c의
 * meaningful_growth/delivery_reason에서 읽는다. */
#define NOTIFICATION_QUEUE_LIMIT 32u
#define NOTIFICATION_TRACK_LIMIT 1024u
#define NOTIFICATION_MEMBER_LIMIT 8u
#define NOTIFICATION_PROGRAM_LIMIT 1024u
#define NOTIFICATION_TTL_MS UINT64_C(90000)
#define NOTIFICATION_SPACING_MS UINT64_C(30000)
#define NOTIFICATION_RETRY_MS UINT64_C(300000)
#define NOTIFICATION_FRESH_MS UINT64_C(15000)
#define NOTIFICATION_JOIN_MS UINT64_C(15000)
#ifndef NOTIFICATION_RECURRENCE_MS
#define NOTIFICATION_RECURRENCE_MS UINT64_C(7200000)
#endif
#define NOTIFICATION_NAME_SIZE 160u

/* 후보가 된 완료창의 값 사본이다. 최신 원시 표본과 별개이며 알림 상세의 당시 근거로 쓴다.
 * 단위는 MonitorMetricReport와 같다. 포인터를 보관하지 않아 snapshot 해제 뒤에도 유효하다. */
typedef struct NotificationMetricEvidence {
    uint64_t window_start, window_end;
    double current, median, threshold, duration_seconds;
    unsigned streak;
} NotificationMetricEvidence;

typedef enum NotificationReason {
    NOTIFICATION_REASON_FIRST = 1, NOTIFICATION_REASON_RECURRENCE,
    NOTIFICATION_REASON_DEFERRED, NOTIFICATION_REASON_GROWTH
} NotificationReason;

typedef struct NotificationMember {
    uint32_t pid;
    unsigned metrics;
    uint64_t creation;
    NotificationMetricEvidence metric[MONITOR_METRICS];
    uint64_t candidate_since[MONITOR_METRICS];
    NotificationReason reason[MONITOR_METRICS];
    unsigned recurrence_count[MONITOR_METRICS];
} NotificationMember;

typedef struct NotificationEvent {
    uint64_t id, queued_ms;
    unsigned session, metrics, member_count, omitted_count, omitted_metrics;
    int64_t program;
    char name[NOTIFICATION_NAME_SIZE];
    NotificationMember members[NOTIFICATION_MEMBER_LIMIT];
} NotificationEvent;

typedef struct NotificationStats {
    uint64_t queued, overflow, tracking_drops, expired, invalidated, feedback_purged;
    uint64_t requests, accepted, failed;
    uint64_t deferred, suppressed_ended, omitted, history_drops;
} NotificationStats;

typedef struct NotificationTrack {
    uint32_t pid;
    unsigned accepted, delivered, provisional, deferred, overflow_metrics;
    uint64_t creation;
    int64_t program;
    uint64_t retry_at[MONITOR_METRICS];
    double reference[MONITOR_METRICS];
    uint64_t growth_end[MONITOR_METRICS], candidate_since[MONITOR_METRICS];
    uint64_t overflow_end[MONITOR_METRICS], overflow_id, overflow_joined_ms;
    unsigned char growth_count[MONITOR_METRICS];
    NotificationReason overflow_reason[MONITOR_METRICS];
} NotificationTrack;

typedef struct NotificationProgram {
    int64_t program;
    uint64_t accepted_at[MONITOR_METRICS];
    unsigned accepted_metrics, ended[MONITOR_METRICS];
} NotificationProgram;

/* 최초 사용 전 0으로 초기화하고 호출자가 모든 접근을 직렬화한다. 정책은 상한이
 * 정해진 값 복사본만 소유하며 동적 할당이나 snapshot 포인터 보관은 하지 않는다. */
typedef struct NotificationPolicy {
    NotificationEvent queue[NOTIFICATION_QUEUE_LIMIT];
    NotificationEvent active;
    NotificationTrack tracks[NOTIFICATION_TRACK_LIMIT];
    NotificationProgram programs[NOTIFICATION_PROGRAM_LIMIT];
    NotificationStats stats;
    uint64_t last_observation_ms, last_sequence, next_request_ms, next_id;
    uint64_t dispatch_id;
    unsigned count, track_count, program_count, session;
    int enabled, active_sent, dispatch_valid, growth_armed;
} NotificationPolicy;

void notification_policy_enable(NotificationPolicy *, int enabled);
void notification_policy_reset(NotificationPolicy *);
/* 필터·접기 전의 전체 COMMIT snapshot을 받는다. 실패·합성 snapshot은 추적 상태를 비운다.
 * 같은 program_id의 후보를 묶으며 GUI의 사용자 그룹 소속은 입력으로 받지 않는다. */
void notification_policy_observe(NotificationPolicy *, const MonitorSnapshot *, unsigned session,
    uint64_t now_ms, uint64_t now_utc);
void notification_policy_feedback(NotificationPolicy *, int64_t program, MonitorFeedback kind,
    uint64_t expires, uint64_t now_utc);

/* 대기·신선도·피드백 조건이 맞으면 큐 첫 건을 active로 옮겨 복사한다. 0이면 out은 그대로다.
 * 아직 Shell 요청은 아니며 호출자는 dispatch에서 다시 확보해야 한다. */
int notification_policy_begin(NotificationPolicy *, uint64_t now_ms, int64_t feedback_in_progress,
    NotificationEvent *out);
/* Shell IPC 직전에 확보한 요청을 다시 검증하고 당시 근거를 복사해 반환한다.
 * dispatch가 성공한 요청마다 결과를 정확히 한 번 보고한다. */
int notification_policy_dispatch(NotificationPolicy *, uint64_t id, uint64_t now_ms,
    int64_t feedback_in_progress, NotificationEvent *out);
/* dispatch가 복사한 동일 event를 한 번 돌려준다. accepted는 Shell 요청 수락 여부이며
 * 사용자 열람 여부가 아니다. 실패면 재시도를 예약하고 active를 퇴역시킨다. */
void notification_policy_result(NotificationPolicy *, const NotificationEvent *, int accepted, uint64_t now_ms);
void notification_policy_retire(NotificationPolicy *, uint64_t id);
/* 송출한 active ID의 당시 사본을 꺼내며 현재 값으로 갱신하지 않는다. 일치 ID는 만료여도
 * 소비하므로 반환값 1일 때만 out을 열고, 실제 인스턴스 재확인은 GUI/bridge가 맡는다. */
int notification_policy_click(NotificationPolicy *, uint64_t id, uint64_t now_ms, NotificationEvent *out);
unsigned notification_cause(const NotificationEvent *);
unsigned notification_member_cause(const NotificationMember *);

#endif
