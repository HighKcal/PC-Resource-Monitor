/* Public-API compatibility probe, intentionally supplies duplicate identities.
 * A synthetic caller may do this today. Record original source/snapshot values
 * separately from the legacy first-matching-row sidecar values. Never turn a
 * successful duplicate input into an expected failure to accommodate a change.
 * Output excludes runtime timings/session wall clock for before/after compare. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "experiment.h"
#include "sqlite3.h"

#define TICKS UINT64_C(10000000)
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%d %s\n", __LINE__, #x); } } while (0)

static void dump(const char *file, const char *label, const char *sql)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *s = NULL;
    int rc = SQLITE_ERROR, column;
    CHECK(sqlite3_open_v2(file, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (!s) goto finished;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        printf("%s", label);
        for (column = 0; column < sqlite3_column_count(s); ++column) {
            switch (sqlite3_column_type(s, column)) {
            case SQLITE_NULL: printf(" |NULL"); break;
            case SQLITE_INTEGER: printf(" |%lld", (long long)sqlite3_column_int64(s, column)); break;
            case SQLITE_FLOAT: printf(" |%.17g", sqlite3_column_double(s, column)); break;
            default: printf(" |%s", (const char *)sqlite3_column_text(s, column)); break;
            }
        }
        printf("\n");
    }
    CHECK(rc == SQLITE_DONE);
finished:
    sqlite3_finalize(s);
    sqlite3_close(db);
}

static void run_case(const char *directory, unsigned kind)
{
    const uint64_t anchor = UINT64_C(134058672000000000);
    ResourceData rows[2];
    ResourceBatch batch;
    Experiment *e = NULL;
    MonitorSnapshot *snapshot = NULL;
    ExperimentDecision *decisions = NULL;
    ExperimentStats stats;
    const char *paths[2] = {"SYNTHETIC:\\duplicate\\first.exe", "SYNTHETIC:\\duplicate\\second.exe"};
    char source[1024], evidence[1024], error[512];
    size_t count = 0, i;
    unsigned step, metric;
    memset(rows, 0, sizeof(rows)); memset(&batch, 0, sizeof(batch));
    snprintf(source, sizeof(source), "%s/duplicate-%u-source.sqlite", directory, kind);
    snprintf(evidence, sizeof(evidence), "%s/duplicate-%u-evidence.sqlite", directory, kind);
    CHECK(GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA(evidence) == INVALID_FILE_ATTRIBUTES);
    CHECK(experiment_create(source, evidence, 1, &e, error, sizeof(error)));
    if (!e) { printf("CREATE %s\n", error); return; }
    batch.items = rows; batch.count = 2; batch.logical_processor_count = 8;
    for (i = 0; i < 2; ++i) {
        rows[i].pid = 9123;
        rows[i].creation_time_utc_100ns = anchor - TICKS;
        rows[i].process_name_utf8 = i && kind ? "second.exe" : "first.exe";
        rows[i].valid_fields = RESOURCE_VALID_IDENTITY | RESOURCE_VALID_NAME | RESOURCE_VALID_CPU |
            RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL;
        rows[i].observation_interval_seconds = rows[i].cpu_interval_seconds = rows[i].io_interval_seconds = 3;
        rows[i].cpu_percent = i && kind ? 80 : 10;
        rows[i].working_set_bytes = (i && kind ? UINT64_C(900) : UINT64_C(100)) * 1024 * 1024;
        rows[i].io_read_bytes_per_second = rows[i].io_write_bytes_per_second = i && kind ? 8000 : 1000;
    }
    if (!kind) paths[1] = paths[0];
    if (kind == 3) rows[0].valid_fields &= ~RESOURCE_VALID_CPU;
    printf("CASE %u\n", kind);
    for (step = 0; step <= 20; ++step) {
        rows[0].measured_at_utc_100ns = anchor + step * 3 * TICKS;
        rows[1].measured_at_utc_100ns = rows[0].measured_at_utc_100ns + (kind == 2 ? 1u : 0u);
        batch.started_at_utc_100ns = rows[0].measured_at_utc_100ns;
        batch.finished_at_utc_100ns = rows[1].measured_at_utc_100ns;
        monitor_snapshot_free(snapshot); snapshot = NULL;
        CHECK(experiment_ingest(e, &batch, paths, &snapshot));
        if (!snapshot) { printf("INGEST step=%u error=%s\n", step, experiment_error(e)); goto finished; }
        CHECK(snapshot->count == 2);
        CHECK(snapshot->completed_windows == (step == 20 ? 2u : 0u));
    }
    for (i = 0; i < snapshot->count; ++i) {
        const MonitorRow *row = &snapshot->rows[i];
        printf("SNAPSHOT row=%zu name=%s path=%s program=%lld\n", i, row->name, row->path, (long long)row->program_id);
        for (metric = 0; metric < MONITOR_METRICS; ++metric) {
            const MonitorMetricReport *r = &row->metric[metric];
            printf("REPORT row=%zu metric=%u state=%d current=%.17g start=%llu end=%llu duration=%.17g covered=%.17g samples=%u eligible=%d\n",
                i, metric, r->state, r->current, (unsigned long long)r->window_start_utc_100ns,
                (unsigned long long)r->window_end_utc_100ns, r->window_duration_seconds, r->covered_seconds,
                r->valid_samples, r->baseline_eligible);
        }
    }
    CHECK(experiment_stats(e, &stats));
    CHECK(stats.windows == 2 && stats.decisions == 16);
    CHECK(stats.source_available && stats.latest_source_available && stats.latest_source_rows == 2);
    CHECK(experiment_decisions(e, EXPERIMENT_PERSONALIZED, 100, 0, &decisions, &count));
    CHECK(count == 8);
    for (i = 0; i < count; ++i) {
        const ExperimentDecision *d = &decisions[i];
        const MonitorMetricReport *legacy = &snapshot->rows[0].metric[d->metric];
        CHECK(strcmp(d->name, snapshot->rows[0].name) == 0);
        CHECK(strcmp(d->path, snapshot->rows[0].path) == 0);
        CHECK(d->report.window_start_utc_100ns == legacy->window_start_utc_100ns);
        CHECK(d->report.window_end_utc_100ns == legacy->window_end_utc_100ns);
        CHECK(d->report.state == legacy->state);
        CHECK((isnan(d->report.current) && isnan(legacy->current)) || d->report.current == legacy->current);
    }
    dump(source, "SOURCE", "SELECT w.id,w.run_id,w.program_id,w.pid,w.creation,w.start_utc,w.end_utc,w.duration,m.metric,m.value,m.covered,m.valid_count,m.state,m.eligible FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id ORDER BY w.id,m.metric");
    dump(evidence, "SIDECAR", "SELECT w.source_window,w.source_run,w.cutoff,w.start,w.end,w.duration,w.name,w.path,d.metric,d.method,m.current,d.state FROM window_evidence w JOIN decision d ON d.window_id=w.id JOIN metric_evidence m ON m.window_id=w.id AND m.metric=d.metric ORDER BY w.id,d.metric,d.method");
finished:
    experiment_decisions_free(decisions, count);
    monitor_snapshot_free(snapshot);
    experiment_close(e);
}

int main(int argc, char **argv)
{
    unsigned kind;
    if (argc != 2) { fprintf(stderr, "usage: test_duplicate_identity OUTPUT_DIRECTORY\n"); return 2; }
    for (kind = 0; kind < 4; ++kind) run_case(argv[1], kind);
    printf("DUPLICATE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
