#ifndef MONITOR_H
#define MONITOR_H
#include "resource_collector.h"
#ifdef __cplusplus
extern "C" {
#endif

#define MONITOR_METRICS 4
typedef struct MonitorEngine MonitorEngine;
typedef enum MonitorMetric { MONITOR_CPU, MONITOR_RAM, MONITOR_READ, MONITOR_WRITE } MonitorMetric;
typedef enum MonitorState { MONITOR_UNAVAILABLE, MONITOR_LEARNING, MONITOR_NORMAL,
    MONITOR_ATTENTION, MONITOR_CANDIDATE } MonitorState;
typedef enum MonitorFeedback { MONITOR_FEEDBACK_CANCEL, MONITOR_FEEDBACK_IGNORE,
    MONITOR_FEEDBACK_EXPECTED } MonitorFeedback;
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
    ResourceData raw; /* process_name_utf8 points to this snapshot's name. */
    char *name, *path;
    int64_t program_id;
    uint32_t path_error;
    MonitorMetricReport metric[MONITOR_METRICS];
    MonitorFeedback feedback;
    uint64_t feedback_expires_utc_100ns;
} MonitorRow;
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

/* Worker-thread-only API. New DBs are tagged; unrecognized/future schemas and
 * real/synthetic mismatches are refused without migration. Path is UTF-8 local
 * disk; caller owns error buffer. NULL config does not imply an in-memory DB. */
int monitor_open(const char *path_utf8, int synthetic, MonitorEngine **out,
    char *error, size_t error_size);
/* One batch, one atomic raw+summary transaction. fixtures must be NULL for real
 * DBs; synthetic fixtures are verified-path substitutes, not real identities.
 * On failure *out=NULL, transaction rolls back, all window/streak state resets.
 * Snapshot and all strings are independent of batch/engine and caller-owned. */
int monitor_ingest(MonitorEngine *, const ResourceBatch *,
    const char *const *synthetic_paths, MonitorSnapshot **out);
int monitor_record_failure(MonitorEngine *, uint64_t utc, unsigned status);
void monitor_reset(MonitorEngine *); /* Discard incomplete windows and streaks. */
int monitor_feedback(MonitorEngine *, int64_t program_id, MonitorFeedback,
    uint64_t now_utc_100ns);
const char *monitor_error(const MonitorEngine *);
const char *monitor_storage_version(void);
/* Optional, read-only worker diagnostic; counts/scans pages and may take time. */
int monitor_storage_stats(MonitorEngine *, MonitorStorageStats *);
void monitor_snapshot_free(MonitorSnapshot *);
void monitor_close(MonitorEngine *);
/* Returns malloc-owned UTF-8 path only after same-handle creation validation.
 * No privilege elevation; creation mismatch returns ERROR_INVALID_DATA. */
char *monitor_query_path(uint32_t pid, uint64_t expected_creation, uint32_t *error);
uint64_t monitor_utc_now(void);
const char *monitor_state_name(MonitorState);
const char *monitor_metric_name(unsigned);
#ifdef __cplusplus
}
#endif
#endif
