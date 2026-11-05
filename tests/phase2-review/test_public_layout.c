/* Compile unchanged against before/current public headers with the same flags.
 * Compare stdout byte-for-byte. This records layout, not function behavior. */
#include <stddef.h>
#include <stdio.h>
#include "monitor.h"
#include "experiment.h"

#define TYPE(t) printf("TYPE %s %zu %zu\n", #t, sizeof(t), _Alignof(t))
#define FIELD(t, f) printf("FIELD %s.%s %zu %zu\n", #t, #f, offsetof(t, f), sizeof(((t *)0)->f))

int main(void)
{
    TYPE(MonitorMetric); TYPE(MonitorState); TYPE(MonitorFeedback);
    printf("ENUM metric %d %d %d %d\n", MONITOR_CPU, MONITOR_RAM, MONITOR_READ, MONITOR_WRITE);
    printf("ENUM state %d %d %d %d %d\n", MONITOR_UNAVAILABLE, MONITOR_LEARNING, MONITOR_NORMAL,
        MONITOR_ATTENTION, MONITOR_CANDIDATE);
    printf("ENUM feedback %d %d %d\n", MONITOR_FEEDBACK_CANCEL, MONITOR_FEEDBACK_IGNORE, MONITOR_FEEDBACK_EXPECTED);
    TYPE(MonitorMetricReport);
    FIELD(MonitorMetricReport, state); FIELD(MonitorMetricReport, current);
    FIELD(MonitorMetricReport, median); FIELD(MonitorMetricReport, mad);
    FIELD(MonitorMetricReport, threshold); FIELD(MonitorMetricReport, covered_seconds);
    FIELD(MonitorMetricReport, count); FIELD(MonitorMetricReport, days);
    FIELD(MonitorMetricReport, minutes); FIELD(MonitorMetricReport, streak);
    FIELD(MonitorMetricReport, valid_samples); FIELD(MonitorMetricReport, window_start_utc_100ns);
    FIELD(MonitorMetricReport, window_end_utc_100ns); FIELD(MonitorMetricReport, window_duration_seconds);
    FIELD(MonitorMetricReport, baseline_eligible); FIELD(MonitorMetricReport, reason);
    TYPE(MonitorRow);
    FIELD(MonitorRow, raw); FIELD(MonitorRow, name); FIELD(MonitorRow, path);
    FIELD(MonitorRow, program_id); FIELD(MonitorRow, path_error); FIELD(MonitorRow, metric);
    FIELD(MonitorRow, feedback); FIELD(MonitorRow, feedback_expires_utc_100ns);
    TYPE(MonitorSnapshot);
    FIELD(MonitorSnapshot, rows); FIELD(MonitorSnapshot, count);
    FIELD(MonitorSnapshot, measured_utc_100ns); FIELD(MonitorSnapshot, sequence);
    FIELD(MonitorSnapshot, foreground_observed_utc_100ns); FIELD(MonitorSnapshot, foreground_pid);
    FIELD(MonitorSnapshot, collect_ms); FIELD(MonitorSnapshot, store_ms);
    FIELD(MonitorSnapshot, completed_windows); FIELD(MonitorSnapshot, synthetic);
    FIELD(MonitorSnapshot, storage_error); FIELD(MonitorSnapshot, collector_error); FIELD(MonitorSnapshot, status);
    TYPE(MonitorStorageStats);
    FIELD(MonitorStorageStats, raw_rows); FIELD(MonitorStorageStats, windows);
    FIELD(MonitorStorageStats, metric_rows); FIELD(MonitorStorageStats, database_page_bytes);
    FIELD(MonitorStorageStats, raw_page_bytes); FIELD(MonitorStorageStats, aggregate_page_bytes);
    TYPE(ResourceData); TYPE(ResourceBatch);
    TYPE(ExperimentDecision); TYPE(ExperimentEpisode); TYPE(ExperimentEvaluation); TYPE(ExperimentStats);
    return 0;
}
