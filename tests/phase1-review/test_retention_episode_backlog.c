/* Independent root-cardinality case. Seed through the actual reducer, then use
 * public ingest/replay. Synthetic source IDs and no live collector or usage. */
#include "../support/experiment_fixture.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%u %s\n", (unsigned)__LINE__, #x); } } while (0)

static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t value = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) value = sqlite3_column_int64(s, 0);
    else CHECK(0);
    sqlite3_finalize(s); return value;
}

int main(int argc, char **argv)
{
    Experiment *e = NULL;
    MonitorSnapshot *snapshot = NULL;
    ResourceBatch batch = {0};
    char source[1024], evidence[1024], replay_path[1024], error[512];
    uint64_t now = monitor_utc_now(), base = now - 40 * DAY;
    unsigned person, window, metric, operation = 0;
    if (argc != 2) return 2;
    snprintf(source, sizeof(source), "%s/episode-backlog-source.sqlite", argv[1]);
    snprintf(evidence, sizeof(evidence), "%s/episode-backlog-evidence.sqlite", argv[1]);
    CHECK(experiment_create(source, evidence, 1, &e, error, sizeof(error)));
    if (!e) { printf("CREATE %s\n", error); return 2; }
    CHECK(experiment_fixture_execute(e, "BEGIN IMMEDIATE"));
    for (person = 0; person < 128; ++person) for (window = 0; window < 4; ++window) {
        MonitorMetricReport reports[MONITOR_METRICS];
        memset(reports, 0, sizeof(reports));
        for (metric = 0; metric < MONITOR_METRICS; ++metric) {
            MonitorMetricReport *r = &reports[metric];
            r->state = window < 3 ? MONITOR_CANDIDATE : MONITOR_NORMAL;
            r->streak = window < 3 ? 3 : 0;
            r->current = window < 3 ? experiment_fixture_fixed_limit(metric) * 2 : 0;
            r->median = 0; r->mad = 0; r->threshold = experiment_fixture_fixed_limit(metric);
            r->covered_seconds = r->window_duration_seconds = 60;
            r->valid_samples = 20; r->baseline_eligible = 1;
            r->window_start_utc_100ns = base + window * 60 * TICKS;
            r->window_end_utc_100ns = r->window_start_utc_100ns + 60 * TICKS;
            snprintf(r->reason, sizeof(r->reason), "independent synthetic episode root backlog");
        }
        ++operation;
        CHECK(experiment_fixture_store_window(e, operation, operation, operation - 1, experiment_fixture_session(e), 4000 + person,
            base - DAY, "root-backlog.exe", "SYNTHETIC:\\root-backlog.exe", reports, 0, 0, 0));
    }
    CHECK(experiment_fixture_execute(e, "COMMIT"));
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM operation") == 512);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=1") == 1024);
    snprintf(replay_path, sizeof(replay_path), "%s/episode-backlog-full-replay.sqlite", argv[1]);
    CHECK(experiment_replay(evidence, replay_path, error, sizeof(error)));
    batch.started_at_utc_100ns = batch.finished_at_utc_100ns = now;
    batch.logical_processor_count = 1;
    CHECK(experiment_ingest(e, &batch, NULL, &snapshot));
    monitor_snapshot_free(snapshot);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") >= 24);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state<>1") == 0);
    printf("ROOT_BACKLOG remaining_operations=%lld episodes=%lld decisions=%lld\n",
        (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM operation"),
        (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode"),
        (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision"));
    snprintf(replay_path, sizeof(replay_path), "%s/episode-backlog-retained-replay.sqlite", argv[1]);
    CHECK(experiment_replay(evidence, replay_path, error, sizeof(error)));
    { sqlite3 *replayed = NULL;
      CHECK(sqlite3_open_v2(replay_path, &replayed, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
      if (replayed) {
          char attach[1200];
          int64_t expected = scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=1");
          int64_t actual = scalar(replayed, "SELECT count(*) FROM episode WHERE state=1");
          CHECK(actual == expected);
          CHECK(scalar(replayed, "SELECT count(*) FROM episode WHERE state=0") == 0);
          snprintf(attach, sizeof(attach), "ATTACH DATABASE '%s' AS original", evidence);
          CHECK(sqlite3_exec(replayed, attach, NULL, NULL, NULL) == SQLITE_OK);
          CHECK(scalar(replayed, "SELECT count(*) FROM(SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying,name,path FROM episode EXCEPT SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying,name,path FROM original.episode)") == 0);
          CHECK(scalar(replayed, "SELECT count(*) FROM(SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying,name,path FROM original.episode EXCEPT SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying,name,path FROM episode)") == 0);
          printf("ROOT_BACKLOG_REPLAY expected_recovered=%lld actual_recovered=%lld\n", (long long)expected, (long long)actual);
          sqlite3_close(replayed);
      }
    }
    CHECK(sqlite3_get_autocommit(experiment_fixture_db(e)));
    CHECK(sqlite3_next_stmt(experiment_fixture_db(e), NULL) == NULL);
    experiment_close(e);
    printf("independent episode root backlog checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
