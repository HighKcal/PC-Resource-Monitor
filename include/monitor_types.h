#ifndef MONITOR_TYPES_H
#define MONITOR_TYPES_H
#include "resource_collector.h"
#ifdef __cplusplus
extern "C" {
#endif

#define MONITOR_METRICS 4
typedef enum MonitorMetric { MONITOR_CPU, MONITOR_RAM, MONITOR_READ, MONITOR_WRITE } MonitorMetric;
typedef enum MonitorState { MONITOR_UNAVAILABLE, MONITOR_LEARNING, MONITOR_NORMAL,
    MONITOR_ATTENTION, MONITOR_CANDIDATE } MonitorState;
typedef enum MonitorFeedback { MONITOR_FEEDBACK_CANCEL, MONITOR_FEEDBACK_IGNORE,
    MONITOR_FEEDBACK_EXPECTED } MonitorFeedback;
/* current·median·mad·threshold는 metric별 단위(CPU %, RAM 바이트, 읽기·쓰기 B/s)를 쓴다.
 * 창 시각은 FILETIME UTC 100ns, duration·coverage는 초다. 상태·품질과 함께 읽는다. */
typedef struct MonitorMetricReport {
    MonitorState state;
    double current, median, mad, threshold, covered_seconds;
    unsigned count, days, minutes, streak, valid_samples;
    uint64_t window_start_utc_100ns, window_end_utc_100ns;
    double window_duration_seconds;
    int baseline_eligible;
    char reason[96];
} MonitorMetricReport;
typedef struct MonitorRow {
    ResourceData raw; /* process_name_utf8은 이 snapshot의 name을 가리킨다. */
    char *name, *path;
    int64_t program_id;
    uint32_t path_error;
    MonitorMetricReport metric[MONITOR_METRICS];
    MonitorFeedback feedback;
    uint64_t feedback_expires_utc_100ns;
} MonitorRow;
/* 호출자가 소유하는 회차 결과. rows·이름·경로까지 monitor_snapshot_free로 해제한다. */
typedef struct MonitorSnapshot {
    MonitorRow *rows;
    size_t count;
    uint64_t measured_utc_100ns, sequence;
    uint64_t foreground_observed_utc_100ns;
    uint32_t foreground_pid;
    double collect_ms, store_ms;
    size_t completed_windows;
    int synthetic, storage_error, collector_error;
    char status[256];
} MonitorSnapshot;
typedef struct MonitorStorageStats {
    uint64_t raw_rows, windows, metric_rows, database_page_bytes;
    uint64_t raw_page_bytes, aggregate_page_bytes;
} MonitorStorageStats;

#ifdef __cplusplus
}
#endif
#endif
