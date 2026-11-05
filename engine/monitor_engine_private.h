#ifndef MONITOR_ENGINE_PRIVATE_H
#define MONITOR_ENGINE_PRIVATE_H
#include "monitor.h"
#include "monitor_window.h"
#include "monitor_store.h"
#include "monitor_source.h"
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
};
#endif
