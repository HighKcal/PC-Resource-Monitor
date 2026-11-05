/* Links only engine/monitor_window.c and the C runtime. Literal expectations
 * verify the arithmetic/quality contract without engine, Baseline or SQLite. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "monitor_window.h"
#define REVIEW_TICKS UINT64_C(10000000)
static const uint64_t anchor = UINT64_C(134058672000000000);
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)

static ResourceData raw(double seconds)
{
    ResourceData r;
    memset(&r, 0, sizeof(r));
    r.valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_OBSERVATION_INTERVAL | RESOURCE_VALID_CPU |
        RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE;
    r.observation_interval_seconds = r.cpu_interval_seconds = r.io_interval_seconds = seconds;
    r.working_set_bytes = 100;
    return r;
}
static void weighted(void)
{
    const double cpu_intervals[] = {9, 12, 12, 15};
    const double writes[] = {1000, 2000, 4000, 8000};
    MonitorWindowAccumulator w = {0};
    MonitorMetricReport report;
    ResourceData r = raw(15);
    unsigned i;
    monitor_window_reset(&w, anchor);
    for (i = 0; i < 4; ++i) {
        MonitorWindowStep step;
        r.measured_at_utc_100ns = anchor + (i + 1) * 15 * REVIEW_TICKS;
        r.cpu_interval_seconds = cpu_intervals[i]; r.cpu_percent = 10 * (double)(i + 1);
        r.working_set_bytes = 100 * (uint64_t)(i + 1);
        r.io_read_bytes_per_second = 100 * (double)(i + 1); r.io_write_bytes_per_second = writes[i];
        step = monitor_window_observe(&w, &r, 1);
        CHECK(!step.reset && step.missing == 0); CHECK(step.complete == (i == 3));
    }
    CHECK(monitor_window_metric(&w, MONITOR_CPU, &report));
    CHECK(report.current == 26.875 && report.covered_seconds == 48); /* 1290 / 48. */
    CHECK(report.window_start_utc_100ns == anchor && report.window_end_utc_100ns == anchor + 60 * REVIEW_TICKS);
    CHECK(report.window_duration_seconds == 60);
    CHECK(!monitor_window_metric(&w, MONITOR_RAM, &report));
    CHECK(report.current == 250 && report.valid_samples == 4); /* Arithmetic mean; insufficient samples. */
    CHECK(monitor_window_metric(&w, MONITOR_READ, &report)); CHECK(report.current == 250);
    CHECK(monitor_window_metric(&w, MONITOR_WRITE, &report)); CHECK(report.current == 3750);
}
static void zero_and_ram(unsigned miss_ram)
{
    MonitorWindowAccumulator w = {0};
    MonitorMetricReport report;
    ResourceData r = raw(4);
    unsigned i;
    monitor_window_reset(&w, anchor);
    for (i = 0; i < 15; ++i) {
        MonitorWindowStep step;
        r.measured_at_utc_100ns = anchor + (i + 1) * 4 * REVIEW_TICKS;
        if (miss_ram && i == 0) r.valid_fields &= ~RESOURCE_VALID_MEMORY;
        else r.valid_fields |= RESOURCE_VALID_MEMORY;
        step = monitor_window_observe(&w, &r, 1);
        CHECK(step.missing == (miss_ram && i == 0 ? 1u << MONITOR_RAM : 0));
        CHECK(step.complete == (i == 14));
    }
    CHECK(monitor_window_metric(&w, MONITOR_CPU, &report)); CHECK(report.current == 0 && report.valid_samples == 15);
    CHECK(monitor_window_metric(&w, MONITOR_READ, &report)); CHECK(report.current == 0);
    CHECK(monitor_window_metric(&w, MONITOR_WRITE, &report)); CHECK(report.current == 0);
    CHECK(monitor_window_metric(&w, MONITOR_RAM, &report) == !miss_ram);
    CHECK(report.current == 100 && report.valid_samples == 15 - miss_ram);
}
static void coverage(unsigned kind)
{
    MonitorWindowAccumulator w = {0};
    MonitorMetricReport report;
    ResourceData r = raw(4);
    unsigned i;
    double expected = kind == 0 ? 45 : kind == 1 ? 75 : 75.1;
    monitor_window_reset(&w, anchor);
    r.cpu_percent = 7;
    for (i = 0; i < 15; ++i) {
        MonitorWindowStep step;
        r.measured_at_utc_100ns = anchor + (i + 1) * 4 * REVIEW_TICKS;
        r.cpu_interval_seconds = kind == 0 ? 3 : kind == 2 && i == 14 ? 5.1 : 5;
        step = monitor_window_observe(&w, &r, 1); CHECK(step.missing == 0 && !step.reset);
    }
    CHECK(monitor_window_metric(&w, MONITOR_CPU, &report) == (kind < 2));
    CHECK(fabs(report.covered_seconds - expected) < 1e-12);
    CHECK(fabs(report.current - 7) < 1e-12);
}
static void resets_and_completion(void)
{
    MonitorWindowAccumulator w = {0};
    ResourceData r = raw(14);
    MonitorWindowStep step;
    unsigned i;
    monitor_window_reset(&w, anchor);
    for (i = 0; i < 4; ++i) {
        r.measured_at_utc_100ns = anchor + (i + 1) * 14 * REVIEW_TICKS;
        step = monitor_window_observe(&w, &r, 1); CHECK(!step.reset && !step.complete);
    }
    r = raw(15); r.measured_at_utc_100ns = anchor + 71 * REVIEW_TICKS;
    step = monitor_window_observe(&w, &r, 1); CHECK(step.complete && !step.reset && w.duration == 71);
    /* Pure helper reports reset; its coordinator performs the reset. */
    step = monitor_window_observe(&w, &r, 1); CHECK(step.reset && !step.complete && w.duration == 71);
    monitor_window_reset(&w, r.measured_at_utc_100ns); CHECK(w.duration == 0 && w.samples[MONITOR_RAM] == 0);
    r = raw(16); r.measured_at_utc_100ns = anchor + 87 * REVIEW_TICKS;
    step = monitor_window_observe(&w, &r, 1); CHECK(step.reset && !step.complete);
    r = raw(3); r.measured_at_utc_100ns = anchor + 90 * REVIEW_TICKS;
    step = monitor_window_observe(&w, &r, 0); CHECK(step.reset && !step.complete);
    r.valid_fields &= ~RESOURCE_VALID_IDENTITY;
    step = monitor_window_observe(&w, &r, 1); CHECK(step.reset && !step.complete);
}
int main(void)
{
    weighted(); zero_and_ram(0); zero_and_ram(1); coverage(0); coverage(1); coverage(2); resets_and_completion();
    printf("WINDOW_STANDALONE checks=%u failures=%u engine_baseline_sqlite_linked=0\n", checks, failures);
    return failures ? 1 : 0;
}
