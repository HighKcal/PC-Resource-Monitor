/* Engine.cs와 맞추는 WPF native ABI다. context와 반환 view의 수명은 아래 계약을,
 * 수집·명령의 실제 처리 순서는 bridge.c를 읽는다. */
#ifndef PC_MONITOR_WPF_BRIDGE_H
#define PC_MONITOR_WPF_BRIDGE_H
#include "monitor.h"
#include "resource_display.h"
#include "program_groups.h"
#include "../../notifications/notification_policy.h"
#include "../../engine/monitor_recent.h"
#include "../../engine/monitor_completed.h"

#define BRIDGE_API __declspec(dllexport)
typedef struct Bridge Bridge;
/* 모든 context 호출은 생성한 worker에서 직렬 실행한다. 반환 포인터는 Bridge 소유이며
 * 다음 상태 변경 호출에서 무효화될 수 있다. managed는 view가 유효한 동안 읽기 전용
 * 조회만 사용하며 다음 상태 변경 전에 값·문자열 복사를 마친다. native 할당의 해제
 * 책임은 DLL에 남으며 CRT 할당을 경계 밖으로 넘기지 않는다. */
typedef struct BridgeView {
    const MonitorSnapshot *snapshot;
    const ResourceDisplayBatch *display;
    const PgSnapshot *relations;
    const PgGroups *groups;
    double grouping_ms;
    unsigned session;
    int grouping_failed, paused;
    uint64_t feedback_revision, target_id;
    const char *target_label;
    const uint64_t *private_working_set;
    int retention_pending;
} BridgeView;

/* mode: 0 실제 수집, 1 저장하는 합성 수집 fixture, 2 알림 전용 demo.
 * 성공한 context는 같은 worker가 bridge_close로 해제한다. 실패 시 부분 자원을
 * 내부에서 정리하고 호출자의 capacity bytes 오류 버퍼에 사유를 복사한다. */
BRIDGE_API Bridge *bridge_open(const char *path_utf8, int mode, char *error, size_t capacity);
BRIDGE_API void bridge_close(Bridge *);
BRIDGE_API const char *bridge_error(Bridge *);
BRIDGE_API int bridge_failed(Bridge *);
BRIDGE_API const BridgeView *bridge_current(Bridge *);
BRIDGE_API const BridgeView *bridge_collect(Bridge *);
/* 같은 session/sequence의 모든 행에 완료창 근거를 한 번에 복사한다. 배열 순서는
 * BridgeView.snapshot과 같고 형식은 monitor_completed.h를 따른다. 기존 view와
 * MonitorMetricReport의 ABI는 바꾸지 않는다. 성공 전 출력은 사용하지 않는다. */
BRIDGE_API int bridge_completed_windows(Bridge *, unsigned session, uint64_t sequence, MonitorCompletedWindow *out, size_t count);
BRIDGE_API int bridge_pause(Bridge *);
BRIDGE_API int bridge_resume(Bridge *);
/* action: 0 그룹 확정, 1 그룹을 이동 대상으로, 2 프로세스를 이동 대상으로, 3 이동,
 * 4 자동 소속에서 분리, 5 자동 소속 복원, 6 펼침, 7 접기, 8/9 전체 펼침/접기.
 * NULL은 편집 자체 거절이다. 편집 적용 뒤 목록 재구성만 실패해도 non-NULL이며,
 * 기존 목록과 새 지정은 유지된다. 이 경우 재적용하지 말고 아래 대기 상태를 읽는다. */
BRIDGE_API const BridgeView *bridge_edit(Bridge *, unsigned session, int action,
    uint32_t pid, uint64_t creation, PgGroupKey group);
/* 같은 worker에서 읽는 0/1 상태다. 적용된 편집을 담은 실제 pg_groups_build 성공
 * 또는 resume의 새 세션에서만 0으로 돌아간다. 현재 view 읽기·재발행은 해제가 아니다.
 * BridgeView의 기존 ABI 배치는 그대로 두고 이 접근자로 상태만 복사한다. */
BRIDGE_API int bridge_group_refresh_pending(Bridge *);
BRIDGE_API int bridge_feedback(Bridge *, unsigned session, uint32_t pid,
    uint64_t creation, int64_t program, MonitorFeedback kind);
BRIDGE_API void bridge_notifications_enable(Bridge *, int enabled);
BRIDGE_API const NotificationEvent *bridge_notification_begin(Bridge *);
BRIDGE_API const NotificationEvent *bridge_notification_dispatch(Bridge *, uint64_t id);
BRIDGE_API void bridge_notification_result(Bridge *, uint64_t id, int accepted);
BRIDGE_API void bridge_notification_retire(Bridge *, uint64_t id);
BRIDGE_API const NotificationEvent *bridge_notification_click(Bridge *, uint64_t id);
BRIDGE_API int64_t bridge_notification_validate(Bridge *, uint64_t id, unsigned session, unsigned member);
BRIDGE_API const BridgeView *bridge_notification_demo(Bridge *);
BRIDGE_API int bridge_storage_stats(Bridge *, MonitorStorageStats *);
BRIDGE_API int bridge_group_expanded(Bridge *, PgGroupKey);
/* 선택한 개별 프로세스 상세에서만 호출한다. 성공에는 완료창이 없는 NO_WINDOW도 포함한다.
 * 오래된 frame/세션/신원/경로는 TARGET_CHANGED로 구분하며 DB 조회 실패와 섞지 않는다.
 * ERROR/TARGET_CHANGED의 출력은 0이고, 소유 worker의 유효 호출은 이번 사유를 error에
 * 남긴다. 실패한 수집 엔진의 원 오류는 보존한다. 다른 스레드는 오류 버퍼를 쓰지 않는다.
 * engine은 완료창 기준 ID가 그대로인 조회 결과를 재사용한다. */
typedef enum BridgeRecentResult {
    BRIDGE_RECENT_ERROR = 0, BRIDGE_RECENT_SUCCESS = 1, BRIDGE_RECENT_TARGET_CHANGED = 2
} BridgeRecentResult;
BRIDGE_API int bridge_recent_result(Bridge *, unsigned session, uint64_t sequence,
    uint32_t pid, uint64_t creation, int64_t program, const char *path, MonitorRecentComparison *out);
/* 기존 호출자의 0/1 계약을 유지한다. TARGET_CHANGED도 0이며 사유는 bridge_error로 읽는다. */
BRIDGE_API int bridge_recent(Bridge *, unsigned session, uint64_t sequence,
    uint32_t pid, uint64_t creation, int64_t program, const char *path, MonitorRecentComparison *out);
/* managed가 native 값을 읽기 전에 확인하는 공개 타입 크기 지문이다. */
BRIDGE_API size_t bridge_sizeof(int type);
#endif
