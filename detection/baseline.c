#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "monitor_internal.h"
#include "storage_internal.h"

#define TICKS MONITOR_TICKS_PER_SECOND
#define DAY MONITOR_DAY_TICKS
#include "baseline.h"
static const double floors[4] = { 5.0, 134217728.0, 1048576.0, 1048576.0 };

static int double_compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static double median(double *v, unsigned n)
{
    qsort(v, n, sizeof(*v), double_compare);
    return n % 2 ? v[n / 2] : v[n / 2 - 1] / 2 + v[n / 2] / 2;
}
int monitor_baseline(MonitorEngine *e, MonitorInstance *p, unsigned metric,
    sqlite3_int64 cutoff, MonitorMetricReport *r)
{
    double values[896], deviations[896];
    uint64_t minute_ids[896];
    unsigned n = 0, days = 0, minutes = 0, age, k;
    uint64_t date = p->start / DAY;
    for (age = 0; age < 14 && date >= age; ++age) {
        sqlite3_stmt *s = e->baseline_query;
        uint64_t lower = (date - age) * DAY, upper = lower + DAY;
        unsigned before = n;
        int rc;
        /* The bounded SQL chain performs the same per-minute index seeks, but
         * binds/resets only once per date. Exact start and cutoff filter every
         * recursive selection. The value uses a scalar PK lookup so an outer
         * metric join cannot turn this into a scan over all program metrics. */
        sqlite3_bind_int(s, 1, (int)metric); sqlite3_bind_int64(s, 2, p->program_id);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)lower); sqlite3_bind_int64(s, 4, (sqlite3_int64)upper);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)p->start); sqlite3_bind_int64(s, 6, cutoff);
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            uint64_t minute;
            double v;
            unsigned j;
            minute = (uint64_t)sqlite3_column_int64(s, 1) / (60 * TICKS);
            v = sqlite3_column_double(s, 0);
            if (n < 896 && isfinite(v) && v >= 0) {
                values[n++] = v;
                for (j = 0; j < minutes && minute_ids[j] != minute; ++j) { }
                if (j == minutes) minute_ids[minutes++] = minute;
            }
        }
        if (rc != SQLITE_DONE) monitor_db_set_error(e, "baseline query");
        sqlite3_reset(s); sqlite3_clear_bindings(s);
        if (rc != SQLITE_DONE) return 0;
        if (n > before) ++days;
    }
    r->count = n; r->days = days; r->minutes = minutes;
    if (n) {
        r->median = median(values, n);
        for (k = 0; k < n; ++k) deviations[k] = fabs(values[k] - r->median);
        r->mad = median(deviations, n);
        r->threshold = r->median + fmax(fmax(3 * 1.4826 * r->mad, 0.5 * r->median), floors[metric]);
    }
    if (n < 60 || days < 3 || minutes < 60) {
        p->streak[metric] = 0; r->state = MONITOR_LEARNING; r->baseline_eligible = 1;
        snprintf(r->reason, sizeof(r->reason), "need 60 windows / 3 UTC dates / 60 distinct minutes");
    } else if (r->current > r->threshold) {
        if (p->streak[metric] < UINT_MAX) ++p->streak[metric];
        r->state = p->streak[metric] >= 3 ? MONITOR_CANDIDATE : MONITOR_ATTENTION;
        r->baseline_eligible = 0;
        snprintf(r->reason, sizeof(r->reason), "upper threshold exceeded; excluded from baseline");
    } else {
        p->streak[metric] = 0; r->state = MONITOR_NORMAL; r->baseline_eligible = 1;
        snprintf(r->reason, sizeof(r->reason), "ready metric at or below upper threshold");
    }
    r->streak = p->streak[metric]; return 1;
}
