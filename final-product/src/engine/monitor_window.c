/* DB와 독립적으로 관측 간격을 누적하고 metric별 창 품질·현재값을 계산한다. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "monitor_window.h"
#include "monitor_policy.h"
static const unsigned metric_bits[4] = { RESOURCE_VALID_CPU, RESOURCE_VALID_MEMORY,
    RESOURCE_VALID_IO_READ, RESOURCE_VALID_IO_WRITE };

void monitor_window_unavailable(MonitorMetricReport *r, const char *reason)
{
    memset(r, 0, sizeof(*r));
    r->state = MONITOR_UNAVAILABLE;
    r->current = r->median = r->mad = r->threshold = NAN;
    snprintf(r->reason, sizeof(r->reason), "%s", reason);
}


void monitor_window_reset(MonitorWindowAccumulator *p, uint64_t anchor)
{
    p->start = p->last_utc = anchor;
    p->duration = 0;
    memset(p->sum, 0, sizeof(p->sum));
    memset(p->covered, 0, sizeof(p->covered));
    memset(p->samples, 0, sizeof(p->samples));
}

MonitorWindowStep monitor_window_observe(MonitorWindowAccumulator *p, const ResourceData *r, int program_known)
{
    double values[4], intervals[4];
    unsigned m;
    MonitorWindowStep step = {0};
    int observation_ok = (r->valid_fields & RESOURCE_VALID_OBSERVATION_INTERVAL) &&
        isfinite(r->observation_interval_seconds) && r->observation_interval_seconds > 0 && r->observation_interval_seconds <= MONITOR_MAX_INTERVAL_SECONDS;
    if (!program_known || !(r->valid_fields & RESOURCE_VALID_IDENTITY)) {
        step.reset = 1;
        return step;
    }
    if (!p->start || !observation_ok || r->measured_at_utc_100ns <= p->last_utc ||
        p->duration + r->observation_interval_seconds > MONITOR_WINDOW_MAX_SECONDS) {
        step.reset = 1;
        return step;
    }
    values[0] = r->cpu_percent;
    values[1] = (double)r->working_set_bytes;
    values[2] = r->io_read_bytes_per_second;
    values[3] = r->io_write_bytes_per_second;
    intervals[0] = r->cpu_interval_seconds;
    intervals[1] = r->observation_interval_seconds;
    intervals[2] = intervals[3] = r->io_interval_seconds;
    /* 창 길이는 관측 QPC 간격, CPU·I/O 가중치는 각 카운터의 QPC 간격을 사용한다.
     * RAM은 유효 표본의 산술평균이므로 합산 시 시간 가중치를 두지 않는다. */
    p->duration += r->observation_interval_seconds;
    p->last_utc = r->measured_at_utc_100ns;
    for (m = 0; m < 4; ++m) {
        if ((r->valid_fields & metric_bits[m]) && isfinite(values[m]) && values[m] >= 0 &&
            isfinite(intervals[m]) && intervals[m] > 0 && intervals[m] <= MONITOR_MAX_INTERVAL_SECONDS) {
            p->sum[m] += values[m] * (m == MONITOR_RAM ? 1 : intervals[m]);
            p->covered[m] += intervals[m];
            ++p->samples[m];
        } else {
            step.missing |= 1u << m;
        }
    }
    step.complete = p->duration >= MONITOR_WINDOW_MIN_SECONDS;
    return step;
}

int monitor_window_metric(const MonitorWindowAccumulator *p, unsigned metric, MonitorMetricReport *report)
{
    int has_coverage = metric == MONITOR_RAM ? p->samples[metric] >= MONITOR_RAM_MIN_SAMPLES :
        p->covered[metric] >= p->duration * MONITOR_MIN_COVERAGE_RATIO &&
        p->covered[metric] <= p->duration * MONITOR_MAX_COVERAGE_RATIO;
    monitor_window_unavailable(report, "insufficient metric coverage or valid sample count");
    report->covered_seconds = p->covered[metric];
    report->valid_samples = p->samples[metric];
    report->window_end_utc_100ns = p->last_utc;
    report->window_start_utc_100ns = p->start;
    report->window_duration_seconds = p->duration;
    if (p->samples[metric] && (metric == MONITOR_RAM || p->covered[metric] > 0))
        report->current = p->sum[metric] /
            (metric == MONITOR_RAM ? p->samples[metric] : p->covered[metric]);
    return has_coverage && isfinite(report->current);
}
