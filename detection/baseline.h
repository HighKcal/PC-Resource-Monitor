#ifndef MONITOR_BASELINE_H
#define MONITOR_BASELINE_H
#include "monitor_internal.h"

/* Compare strictly past history using the transaction cutoff fixed by engine. */
int monitor_baseline(MonitorEngine *, MonitorInstance *, unsigned metric,
    sqlite3_int64 cutoff, MonitorMetricReport *);
#endif
