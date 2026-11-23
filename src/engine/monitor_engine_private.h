#ifndef MONITOR_ENGINE_PRIVATE_H
#define MONITOR_ENGINE_PRIVATE_H
#include "monitor.h"
#include "monitor_window.h"
#include "monitor_store.h"
#include "monitor_source.h"
#include "monitor_recent.h"
#include "monitor_completed.h"
#include "baseline_history.h"
/* engine 구현만 사용하는 상태 배치. path·instances·store·history는 engine이 소유한다.
 * committed_snapshot·committed_rows는 호출자 소유 결과를 빌리며 engine에서 해제하지 않는다. */
typedef struct MonitorInstance {
    uint32_t pid, path_error;
    uint64_t creation;
    int64_t program_id;
    char *path;
    MonitorWindowAccumulator window;
    unsigned streak[MONITOR_METRICS];
    MonitorMetricReport report[MONITOR_METRICS];
    /* 완료 당시의 작은 표시 근거다. 현재 결측/긴 간격이 report를 초기화해도
     * 같은 신원·앞으로 간 시간이면 보존한다. 후보·알림은 계속 report를 읽는다. */
    MonitorCompletedWindow completed;
    /* UTC 순서가 아닌 마지막 유효 저장 창의 ID. 일시 결측과 UTC 역행 중에도 유지한다.
     * lookup_mask는 그 ID(없음 포함)를 확인한 지표만 나타낸다. */
    int64_t recent_anchor[MONITOR_METRICS];
    unsigned recent_lookup_mask;
} MonitorInstance;
struct MonitorEngine {
    MonitorStore *store;
    BaselineHistoryScope history;
    MonitorInstance *instances;
    size_t count;
    int synthetic;
    int64_t session, sequence;
    char error[256];
    uint64_t cookie, mutation_epoch;
    int refs_disabled;
    MonitorCommitRef committed_ref;
    const MonitorSnapshot *committed_snapshot;
    const MonitorRow *committed_rows;
    size_t committed_count, committed_windows;
    uint64_t committed_utc, committed_foreground_utc;
    uint32_t committed_foreground_pid;
    /* 마지막 정리 실패 여부다. 회차/피드백 자체 실패와 별개이며, 다음 성공한 쓰기 뒤
     * 정리까지 성공해야 해제된다. reset으로 경고를 숨기지 않는다. */
    int retention_pending;
    int64_t recent_first_run; /* reset 뒤 첫 성공 회차. 이전 관측 구간의 최근 이력을 차단한다. */
    uint32_t recent_cached_pid;
    uint64_t recent_cached_creation;
    int64_t recent_cached_program, recent_cache_anchor[MONITOR_METRICS];
    unsigned recent_cache_mask;
    MonitorRecentComparison recent_cache; /* 선택 인스턴스 하나의 실제 anchor별 조회 결과 사본. */
};
#endif
