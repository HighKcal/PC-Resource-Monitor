/* Synthetic retention/replay regression. Product reducer seeds complete
 * evidence; fake source IDs do not model collector or long-term usage. The
 * original structural-review defect fixture remains unchanged. */
#include "../support/experiment_fixture.h"
#define RETAIN(e, utc) experiment_fixture_trim((e), (utc))

static unsigned checks, failures, serial, replay_serial;
static const char *output_dir;
static int expect_defect, profile_enabled;
static unsigned profile_sequence;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%u %s\n", (unsigned)__LINE__, #x); } } while (0)

static int64_t scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t result = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) result = sqlite3_column_int64(s, 0);
    else CHECK(0);
    sqlite3_finalize(s);
    return result;
}
static void sql(Experiment *e, const char *text)
{
    int ok = experiment_fixture_execute(e, text);
    CHECK(ok);
    if (!ok) printf("SQL_ERROR %s\n", experiment_error(e));
}
static void paths(char *source, char *evidence, const char *label)
{
    ++serial;
    snprintf(source, 1024, "%s/%u-%s-source.sqlite", output_dir, serial, label);
    snprintf(evidence, 1024, "%s/%u-%s-evidence.sqlite", output_dir, serial, label);
    CHECK(GetFileAttributesA(source) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA(evidence) == INVALID_FILE_ATTRIBUTES);
}
static Experiment *create(char *evidence, const char *label)
{
    Experiment *e = NULL;
    char source[1024], error[512];
    paths(source, evidence, label);
    CHECK(experiment_create(source, evidence, 1, &e, error, sizeof(error)));
    if (!e) { printf("CREATE_ERROR %s\n", error); exit(2); }
    return e;
}
static void idle(Experiment *e)
{
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_get_autocommit(experiment_fixture_db(e)));
    while ((s = sqlite3_next_stmt(experiment_fixture_db(e), s)) != NULL) CHECK(!sqlite3_stmt_busy(s));
    CHECK(sqlite3_next_stmt(experiment_fixture_db(e), NULL) == NULL);
}
static void references(Experiment *e)
{
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision d WHERE d.episode_id IS NOT NULL AND d.episode_id NOT IN(SELECT id FROM episode)") == 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM reduction WHERE episode<>0 AND episode NOT IN(SELECT id FROM episode)") == 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation WHERE (episode IS NOT NULL AND episode NOT IN(SELECT id FROM episode)) OR (decision IS NOT NULL AND decision NOT IN(SELECT id FROM decision))") == 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT integrity_check='ok' FROM pragma_integrity_check") == 1);
}
/* end_state is active/recovered/unknown. A private active seed has no Track;
 * public empty capture therefore does not add an artificial absence gap. */
static void seed(Experiment *e, unsigned count, uint64_t base, int end_state)
{
    unsigned i, m;
    MonitorMetricReport reports[MONITOR_METRICS];
    sql(e, "BEGIN IMMEDIATE");
    for (i = 0; i < count; ++i) {
        memset(reports, 0, sizeof(reports));
        for (m = 0; m < MONITOR_METRICS; ++m) {
            reports[m].state = MONITOR_LEARNING;
            reports[m].current = m == MONITOR_CPU ? 40 : m == MONITOR_RAM ? 67108864 : 0;
            if (m == MONITOR_CPU && i + 1 == count && end_state != EXPERIMENT_ACTIVE) {
                reports[m].current = end_state == EXPERIMENT_RECOVERED ? 0 : NAN;
                if (end_state == EXPERIMENT_UNKNOWN) reports[m].state = MONITOR_UNAVAILABLE;
            }
            reports[m].median = reports[m].mad = reports[m].threshold = NAN;
            reports[m].covered_seconds = reports[m].window_duration_seconds = 60;
            reports[m].valid_samples = 20; reports[m].baseline_eligible = 1;
            reports[m].window_start_utc_100ns = base + (uint64_t)i * 60 * TICKS;
            reports[m].window_end_utc_100ns = reports[m].window_start_utc_100ns + 60 * TICKS;
            snprintf(reports[m].reason, sizeof(reports[m].reason), "synthetic retention regression");
        }
        CHECK(experiment_fixture_store_window(e, i + 1, i + 1, i, experiment_fixture_session(e), 1234, base - DAY,
            "retention.exe", "SYNTHETIC:\\retention.exe", reports, 0, 0, 0));
    }
    sql(e, "COMMIT");
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM window_evidence") == count);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 1);
    CHECK(scalar(experiment_fixture_db(e), "SELECT state FROM episode") == end_state);
}
static int public_trim(Experiment *e, uint64_t now)
{
    MonitorSnapshot *snapshot = NULL;
    ResourceBatch batch;
    int ok;
    memset(&batch, 0, sizeof(batch));
    batch.started_at_utc_100ns = batch.finished_at_utc_100ns = now;
    batch.logical_processor_count = 1;
    ok = experiment_ingest(e, &batch, NULL, &snapshot);
    CHECK(!ok || (snapshot && snapshot->count == 0));
    if (!ok) CHECK(snapshot == NULL);
    monitor_snapshot_free(snapshot);
    return ok;
}
static void replay(const char *evidence, int success)
{
    char destination[1200], error[512];
    int ok;
    snprintf(destination, sizeof(destination), "%s-replay-%u.sqlite", evidence, ++replay_serial);
    ok = experiment_replay(evidence, destination, error, sizeof(error));
    CHECK(ok == success);
    printf("REPLAY expected=%d actual=%d error=%s\n", success, ok, error);
    if (ok) {
        sqlite3 *db = NULL;
        char attach[1600];
        CHECK(sqlite3_open_v2(destination, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
        snprintf(attach, sizeof(attach), "ATTACH DATABASE '%s' AS original", evidence);
        CHECK(sqlite3_exec(db, attach, NULL, NULL, NULL) == SQLITE_OK);
        CHECK(scalar(db, "SELECT count(*) FROM(SELECT * FROM decision EXCEPT SELECT * FROM original.decision)") == 0);
        CHECK(scalar(db, "SELECT count(*) FROM(SELECT * FROM original.decision EXCEPT SELECT * FROM decision)") == 0);
        /* reason describes boundary replay, so compare the numeric projection. */
        CHECK(scalar(db, "SELECT count(*) FROM(SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying FROM episode EXCEPT SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying FROM original.episode)") == 0);
        CHECK(scalar(db, "SELECT count(*) FROM(SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying FROM original.episode EXCEPT SELECT id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying FROM episode)") == 0);
        sqlite3_close(db);
    }
}
static int trace_profile(unsigned event, void *context, void *statement, void *elapsed)
{
    sqlite3_stmt *s = statement;
    const char *text = sqlite3_sql(s);
    (void)context; (void)elapsed;
    if (event == SQLITE_TRACE_PROFILE && profile_enabled && text &&
        (!strncmp(text, "DELETE FROM", 11) || !strncmp(text, "UPDATE retention_state", 22) || !strncmp(text, "WITH rated_episode", 18)))
        printf("RETENTION_COUNTER seq=%u vm=%d fullscan=%d sort=%d stmt_bytes=%d sql=%s\n", ++profile_sequence,
            sqlite3_stmt_status(s, SQLITE_STMTSTATUS_VM_STEP, 0), sqlite3_stmt_status(s, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0),
            sqlite3_stmt_status(s, SQLITE_STMTSTATUS_SORT, 0), sqlite3_stmt_status(s, SQLITE_STMTSTATUS_MEMUSED, 0), text);
    return 0;
}
static void backlog(unsigned count, int state)
{
    char evidence[1024], label[128];
    uint64_t now = monitor_utc_now();
    int64_t windows, before;
    unsigned rounds = 0;
    Experiment *e;
    snprintf(label, sizeof(label), "backlog-%u-state-%d", count, state);
    e = create(evidence, label); seed(e, count, now - 40 * DAY, state);
    replay(evidence, 1);
    sqlite3_trace_v2(experiment_fixture_db(e), SQLITE_TRACE_PROFILE, trace_profile, NULL);
    profile_enabled = 1;
    CHECK(public_trim(e, now));
    profile_enabled = 0;
    windows = scalar(experiment_fixture_db(e), "SELECT count(*) FROM window_evidence");
    CHECK(windows == (count > 1000 ? count - 1000 : 0));
    CHECK(scalar(experiment_fixture_db(e), "SELECT expired_prefix FROM retention_state") == 1);
    if (expect_defect) {
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 0);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision WHERE episode_id IS NOT NULL") > 0);
        replay(evidence, 0);
        printf("EXPECTED_DEFECT backlog=%u missing_episode=retained-decision\n", count);
    } else {
        references(e); replay(evidence, 1);
        while (windows && rounds++ < (count + 999) / 1000 + 1) {
            before = windows; CHECK(public_trim(e, now + rounds));
            windows = scalar(experiment_fixture_db(e), "SELECT count(*) FROM window_evidence");
            CHECK(windows < before && before - windows <= 1000);
            references(e); replay(evidence, 1);
        }
        CHECK(windows == 0);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 0);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM reduction") == 0);
    }
    idle(e); experiment_close(e);
    printf("BACKLOG count=%u state=%d rounds=%u path=%s\n", count, state, rounds + 1, evidence);
}
static void evaluations(unsigned count, int target, int state, int ended_by_gap)
{
    Experiment *e;
    ExperimentEvaluation v;
    char evidence[1024], label[128];
    uint64_t now = monitor_utc_now();
    unsigned i;
    int64_t episode, decision;
    snprintf(label, sizeof(label), "evaluations-%u-target-%d-state-%d-gap-%d", count, target, state, ended_by_gap);
    e = create(evidence, label); seed(e, 4, now - 40 * DAY, ended_by_gap ? EXPERIMENT_ACTIVE : state);
    if (ended_by_gap) {
        CHECK(experiment_fixture_gap(e, now - 40 * DAY + 5 * 60 * TICKS, experiment_fixture_session(e), 1234, now - 41 * DAY, 1u << MONITOR_CPU,
            "synthetic gap-ended episode"));
        CHECK(scalar(experiment_fixture_db(e), "SELECT state FROM episode") == EXPERIMENT_UNKNOWN);
    }
    episode = scalar(experiment_fixture_db(e), "SELECT id FROM episode");
    decision = scalar(experiment_fixture_db(e), "SELECT last_decision FROM episode");
    memset(&v, 0, sizeof(v));
    v.episode_id = target != 1 ? episode : 0; v.decision_id = target != 0 ? decision : 0;
    v.trial_id = "synthetic-retention-test"; v.order_id = "test"; v.judgment = EXPERIMENT_UNSURE;
    v.understood = v.target_correct = v.time_correct = v.metric_correct = v.next_check = -1;
    v.elapsed_seconds = -1;
    sql(e, "BEGIN IMMEDIATE");
    for (i = 0; i < count; ++i) CHECK(experiment_evaluate(e, &v));
    sql(e, "COMMIT");
    sqlite3_trace_v2(experiment_fixture_db(e), SQLITE_TRACE_PROFILE, trace_profile, NULL);
    profile_enabled = 1;
    CHECK(public_trim(e, now));
    profile_enabled = 0;
    if (expect_defect) {
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == 1);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 0);
        printf("EXPECTED_DEFECT episode-only-evaluation-orphan=1\n");
    } else {
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == (count > 1000 ? count - 1000 : 0));
        if (count > 1000) {
            CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 1);
            CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM window_evidence") == 2);
            if (ended_by_gap) CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM operation WHERE kind=1") == 1);
        }
        references(e); replay(evidence, 1);
        CHECK(public_trim(e, now + 1)); references(e);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == 0);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 0);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM window_evidence") == 0);
    }
    idle(e); experiment_close(e);
    printf("EVALUATIONS count=%u target=%d state=%d gap=%d path=%s\n", count, target, state, ended_by_gap, evidence);
}
static void reduction_and_orphan_backlogs(void)
{
    char evidence[1024];
    uint64_t now = monitor_utc_now();
    Experiment *e = create(evidence, "reduction-backlog");
    seed(e, 4, now - 40 * DAY, EXPERIMENT_ACTIVE);
    sql(e, "BEGIN IMMEDIATE; WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1004) INSERT INTO episode SELECT NULL,session,pid+x,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying,name,path,reason FROM episode CROSS JOIN n WHERE id=1; DELETE FROM reduction; INSERT INTO reduction SELECT session,pid,creation,metric,method,qualifying,first,id FROM episode; COMMIT");
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM reduction") == 1005);
    CHECK(public_trim(e, now)); references(e);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM reduction") == 5);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 5);
    CHECK(public_trim(e, now + 1)); references(e);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM reduction") == 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 0);
    sql(e, "WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1005) INSERT INTO evaluation(episode,decision,trial,ordering,judgment,understood,target_correct,time_correct,metric_correct,next_check,elapsed,note,created) SELECT 999999,999999,'synthetic-orphan','test',0,-1,-1,-1,-1,-1,NULL,'',9223372036854775807 FROM n");
    CHECK(public_trim(e, now + 2));
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == 5);
    CHECK(public_trim(e, now + 3)); references(e);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == 0);
    idle(e); experiment_close(e);
}
static void retained(void)
{
    char evidence[1024];
    uint64_t now = monitor_utc_now();
    Experiment *e = create(evidence, "fully-retained");
    seed(e, 8, now - DAY, EXPERIMENT_RECOVERED);
    CHECK(public_trim(e, now));
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM window_evidence") == 8);
    CHECK(scalar(experiment_fixture_db(e), "SELECT expired_prefix FROM retention_state") == 0);
    references(e); replay(evidence, 1); idle(e); experiment_close(e);
}
static void summary_only_roots_after_earlier_global_gap(void)
{
    char evidence[1024];
    uint64_t now = monitor_utc_now(), base = now - 40 * DAY;
    Experiment *e = create(evidence, "summary-only-mixed-with-earlier-rated-gap");
    ExperimentEvaluation v;
    unsigned person, window, metric, operation = 5, i;
    /* One rated earlier episode keeps its ending global gap alive. The later
     * episodes have no retained decisions after bounded operation cleanup. */
    seed(e, 4, base - 10 * 60 * TICKS, EXPERIMENT_ACTIVE);
    CHECK(experiment_fixture_gap(e, base - 5 * 60 * TICKS, 0, 0, 0, ALL_METRICS, "earlier global gap retained by rating"));
    memset(&v, 0, sizeof(v)); v.episode_id = 1;
    v.trial_id = "synthetic-earlier-gap"; v.order_id = "test"; v.judgment = EXPERIMENT_UNSURE;
    v.understood = v.target_correct = v.time_correct = v.metric_correct = v.next_check = -1;
    v.elapsed_seconds = -1;
    sql(e, "BEGIN IMMEDIATE");
    for (i = 0; i < 1005; ++i) CHECK(experiment_evaluate(e, &v));
    for (person = 0; person < 129; ++person) for (window = 0; window < 4; ++window) {
        MonitorMetricReport reports[MONITOR_METRICS];
        memset(reports, 0, sizeof(reports));
        for (metric = 0; metric < MONITOR_METRICS; ++metric) {
            MonitorMetricReport *r = &reports[metric];
            int terminal = window == 3 && person % 3 != 2;
            r->state = terminal ? (person % 3 == 0 ? MONITOR_NORMAL : MONITOR_UNAVAILABLE) : MONITOR_CANDIDATE;
            r->streak = terminal ? 0 : 3;
            r->current = terminal ? (person % 3 == 0 ? 0 : NAN) : experiment_fixture_fixed_limit(metric) * 2;
            r->median = r->mad = 0; r->threshold = experiment_fixture_fixed_limit(metric);
            r->covered_seconds = r->window_duration_seconds = 60;
            r->valid_samples = 20; r->baseline_eligible = 1;
            r->window_start_utc_100ns = base + window * 60 * TICKS;
            r->window_end_utc_100ns = r->window_start_utc_100ns + 60 * TICKS;
            snprintf(r->reason, sizeof(r->reason), "synthetic mixed summary-only roots");
        }
        ++operation;
        CHECK(experiment_fixture_store_window(e, operation, operation, operation - 1, experiment_fixture_session(e), 4000 + person, base - DAY,
            "summary-root.exe", "SYNTHETIC:\\summary-root.exe", reports, 0, 0, 0));
    }
    sql(e, "COMMIT");
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 1033);
    replay(evidence, 1);
    CHECK(public_trim(e, now)); references(e);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == 5);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM operation WHERE kind=1") == 1);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode p WHERE id NOT IN(SELECT episode_id FROM decision WHERE episode_id IS NOT NULL)") > 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=0") > 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=1") > 0);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=2 AND id<>1") > 0);
    replay(evidence, 1); /* full decision links and every numeric summary field */
    printf("SUMMARY_ONLY_REPLAY active=%lld recovered=%lld unknown=%lld earlier_global_gap=retained\n",
        (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=0"),
        (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=1"),
        (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode WHERE state=2"));
    idle(e); experiment_close(e);
}
typedef struct Denial { const char *table; unsigned hits; } Denial;
static int deny_delete(void *context, int action, const char *one, const char *two, const char *three, const char *four)
{
    Denial *d = context;
    (void)two; (void)three; (void)four;
    if ((action == SQLITE_DELETE || action == SQLITE_UPDATE) && one && !strcmp(one, d->table)) {
        ++d->hits; return SQLITE_DENY;
    }
    return SQLITE_OK;
}
static void failure_boundaries(void)
{
    static const char *tables[] = {"evaluation", "retention_state", "operation", "reduction", "episode"};
    unsigned i;
    for (i = 0; i < sizeof(tables) / sizeof(tables[0]) + 1; ++i) {
        char evidence[1024], label[128];
        uint64_t now = monitor_utc_now();
        Experiment *e;
        Denial denial = {i < 5 ? tables[i] : "", 0};
        snprintf(label, sizeof(label), "failure-%u", i);
        e = create(evidence, label); seed(e, 4, now - 40 * DAY, EXPERIMENT_RECOVERED);
        if (i < 5) CHECK(sqlite3_set_authorizer(experiment_fixture_db(e), deny_delete, &denial) == SQLITE_OK);
        else sql(e, "CREATE TEMP TRIGGER fail_retention BEFORE DELETE ON episode BEGIN SELECT RAISE(ABORT,'retention injected step failure'); END");
        CHECK(!public_trim(e, now));
        if (i < 5) { CHECK(denial.hits == 1); CHECK(sqlite3_set_authorizer(experiment_fixture_db(e), NULL, NULL) == SQLITE_OK); }
        else sql(e, "DROP TRIGGER fail_retention");
        CHECK(experiment_fixture_fault(e) == 1);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM operation") == 4);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision") == 32);
        CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM episode") == 1);
        CHECK(scalar(experiment_fixture_db(e), "SELECT expired_prefix FROM retention_state") == 0);
        CHECK(scalar(experiment_fixture_core_db(e), "SELECT count(*) FROM collection_run") == 1);
        references(e); idle(e);
        CHECK(experiment_reset(e, now, "synthetic retry after retention failure"));
        CHECK(public_trim(e, now + 1)); references(e); idle(e); experiment_close(e);
        printf("FAILURE_BOUNDARY stage=%u rollback=ok core_commit=retained\n", i);
    }
}
static void query_profile(Experiment *e, const char *label, const char *query, uint64_t cutoff)
{
    sqlite3_stmt *s = NULL;
    char explain[4096];
    int rc, rows = 0;
    snprintf(explain, sizeof(explain), "EXPLAIN QUERY PLAN %s", query);
    CHECK(sqlite3_prepare_v2(experiment_fixture_db(e), explain, -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW)
        printf("QUERY_PLAN label=%s detail=%s\n", label, experiment_fixture_column_text(s, 3));
    CHECK(rc == SQLITE_DONE); sqlite3_finalize(s); s = NULL;
    CHECK(sqlite3_prepare_v2(experiment_fixture_db(e), query, -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) ++rows;
    CHECK(rc == SQLITE_DONE);
    printf("QUERY_COUNTER label=%s rows=%d vm=%d fullscan=%d sort=%d stmt_bytes=%d\n", label, rows,
        sqlite3_stmt_status(s, SQLITE_STMTSTATUS_VM_STEP, 0), sqlite3_stmt_status(s, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0),
        sqlite3_stmt_status(s, SQLITE_STMTSTATUS_SORT, 0), sqlite3_stmt_status(s, SQLITE_STMTSTATUS_MEMUSED, 0));
    sqlite3_finalize(s);
}
static void plans(void)
{
    char evidence[1024];
    Experiment *e = create(evidence, "query-plan-only");
    /* A separate SQL-only dataset compares anti-reference query costs. No
     * replay or semantic equivalence claims are made from this dataset. */
    sql(e, "BEGIN IMMEDIATE; WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<20000) INSERT INTO operation SELECT x,0,1,1,1,1,0,'' FROM n; INSERT INTO window_evidence SELECT id,id,id,0,0,1,60,'plan','plan',0,0 FROM operation; WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1000) INSERT INTO episode SELECT x,1,1,1,0,1,1,1,1,1,1,1,1,3,'plan','plan','' FROM n; INSERT INTO decision SELECT id,id,0,1,3,3,25,(id%500)+1 FROM operation; COMMIT");
    query_profile(e, "not-in", "SELECT id FROM episode p WHERE max(last,ended)<? AND id NOT IN(SELECT episode_id FROM decision WHERE episode_id IS NOT NULL) AND id NOT IN(SELECT episode FROM reduction WHERE episode<>0) AND id NOT IN(SELECT episode FROM evaluation WHERE episode IS NOT NULL) LIMIT 1000", 2);
    query_profile(e, "not-exists", "SELECT id FROM episode p WHERE max(last,ended)<? AND NOT EXISTS(SELECT 1 FROM decision d WHERE d.episode_id=p.id) AND NOT EXISTS(SELECT 1 FROM reduction r WHERE r.episode=p.id) AND NOT EXISTS(SELECT 1 FROM evaluation v WHERE v.episode=p.id) LIMIT 1000", 2);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM sqlite_schema WHERE type='index' AND name NOT LIKE 'sqlite_%'") == 3);
    idle(e); experiment_close(e);
    e = create(evidence, "no-expiry-132000-decisions");
    sql(e, "BEGIN IMMEDIATE; WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<16500) INSERT INTO operation SELECT x,0,2,1,1,1,0,'' FROM n; INSERT INTO window_evidence SELECT id,id,id,0,1,2,60,'plan','plan',0,0 FROM operation; INSERT INTO decision(window_id,metric,method,state,streak,threshold) SELECT o.id,m.x,p.x,0,0,25 FROM operation o CROSS JOIN(SELECT 0 x UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3) m CROSS JOIN(SELECT 0 x UNION ALL SELECT 1) p; INSERT INTO evaluation(episode,decision,trial,ordering,judgment,understood,target_correct,time_correct,metric_correct,next_check,elapsed,note,created) SELECT NULL,id,'synthetic-no-expiry','test',0,-1,-1,-1,-1,-1,NULL,'',2 FROM decision WHERE id<=2000; COMMIT");
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision") == 132000);
    sqlite3_trace_v2(experiment_fixture_db(e), SQLITE_TRACE_PROFILE, trace_profile, NULL);
    profile_enabled = 1;
    sql(e, "BEGIN IMMEDIATE"); CHECK(RETAIN(e, 28 * DAY + 2)); sql(e, "COMMIT");
    profile_enabled = 0;
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision") == 132000);
    CHECK(scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation") == 2000);
    CHECK(scalar(experiment_fixture_db(e), "SELECT expired_prefix FROM retention_state") == 0);
    idle(e); experiment_close(e);
}
/* The same fixed invariants deliberately fail twice against the original
 * source. Keep this small mode separate from the historical defect oracle. */
static void regression_pair(void)
{
    char evidence[1024], destination[1200], error[512];
    uint64_t now = monitor_utc_now();
    Experiment *e = create(evidence, "paired-retained-replay");
    ExperimentEvaluation v;
    int ok;
    int64_t orphan;
    seed(e, 1005, now - 40 * DAY, EXPERIMENT_RECOVERED);
    CHECK(public_trim(e, now));
    snprintf(destination, sizeof(destination), "%s-paired-replay.sqlite", evidence);
    ok = experiment_replay(evidence, destination, error, sizeof(error));
    CHECK(ok);
    printf("REGRESSION_PAIR invariant=retained_public_replay expected=1 actual=%d missing_episode_decisions=%lld error=%s\n",
        ok, (long long)scalar(experiment_fixture_db(e), "SELECT count(*) FROM decision WHERE episode_id IS NOT NULL AND episode_id NOT IN(SELECT id FROM episode)"), error);
    idle(e); experiment_close(e);
    e = create(evidence, "paired-episode-evaluation");
    seed(e, 4, now - 40 * DAY, EXPERIMENT_RECOVERED);
    memset(&v, 0, sizeof(v)); v.episode_id = 1;
    v.trial_id = "synthetic-paired-fixed-invariant"; v.order_id = "test"; v.judgment = EXPERIMENT_UNSURE;
    v.understood = v.target_correct = v.time_correct = v.metric_correct = v.next_check = -1;
    v.elapsed_seconds = -1;
    CHECK(experiment_evaluate(e, &v)); CHECK(public_trim(e, now));
    orphan = scalar(experiment_fixture_db(e), "SELECT count(*) FROM evaluation WHERE episode IS NOT NULL AND episode NOT IN(SELECT id FROM episode)");
    CHECK(orphan == 0);
    printf("REGRESSION_PAIR invariant=no_episode_evaluation_orphan expected=0 actual=%lld\n", (long long)orphan);
    idle(e); experiment_close(e);
}
int main(int argc, char **argv)
{
    unsigned i;
    static const unsigned sizes[] = {999, 1000, 1001, 1005, 5005};
    if (argc != 3) return 2;
    output_dir = argv[1]; expect_defect = !strcmp(argv[2], "--expect-defect");
    if (!strcmp(argv[2], "--profile-only")) plans();
    else if (!strcmp(argv[2], "--regression-pair")) regression_pair();
    else if (expect_defect) { backlog(1005, EXPERIMENT_RECOVERED); evaluations(1, 0, EXPERIMENT_RECOVERED, 0); }
    else if (!strcmp(argv[2], "--expect-fixed")) {
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) backlog(sizes[i], EXPERIMENT_RECOVERED);
        backlog(1005, EXPERIMENT_ACTIVE); backlog(1005, EXPERIMENT_UNKNOWN);
        evaluations(1, 0, EXPERIMENT_RECOVERED, 0);
        evaluations(1005, 0, EXPERIMENT_RECOVERED, 0); evaluations(1005, 1, EXPERIMENT_RECOVERED, 0);
        evaluations(1005, 2, EXPERIMENT_RECOVERED, 0); evaluations(1005, 0, EXPERIMENT_ACTIVE, 0);
        evaluations(1005, 0, EXPERIMENT_UNKNOWN, 0); evaluations(1005, 0, EXPERIMENT_UNKNOWN, 1);
        evaluations(1005, 1, EXPERIMENT_UNKNOWN, 1);
        retained(); summary_only_roots_after_earlier_global_gap(); reduction_and_orphan_backlogs(); failure_boundaries();
    } else return 2;
    printf("RETENTION_RESULT mode=%s checks=%u failures=%u\n", argv[2], checks, failures);
    return failures ? 1 : 0;
}
