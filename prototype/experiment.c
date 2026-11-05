/* 수집 수명과 두 DB의 저장 순서를 조율한다. core commit 뒤 실험 DB가 실패할 수 있으므로
 * 준비 상태와 snapshot은 실험 DB COMMIT 뒤에만 공개하고 부분 실패는 fault로 남긴다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include "experiment_internal.h"
#include "experiment_retention.h"
#include "monitor_source.h"
const char *experiment_error(const Experiment *e)
{
    return e ? e->error : "experiment unavailable";
}
const char *experiment_method_name(ExperimentMethod m)
{
    return m == EXPERIMENT_PERSONALIZED ? "personalized" : m == EXPERIMENT_FIXED ? "fixed" : "unknown";
}
const char *experiment_episode_state_name(ExperimentEpisodeState s)
{
    return s == EXPERIMENT_ACTIVE      ? "active"
           : s == EXPERIMENT_RECOVERED ? "observed recovery"
                                       : "unknown after observation break";
}
const char *experiment_judgment_name(ExperimentJudgment j)
{
    static const char *const names[] = {"unrated", "useful", "known", "unnecessary", "unsure"};
    return j >= EXPERIMENT_UNRATED && j <= EXPERIMENT_UNSURE ? names[j] : "unknown";
}
int experiment_open_history(const char *path, Experiment **out, char *error, size_t size)
{
    return exp_store_open_sidecar(path, 0, out, error, size);
}
int experiment_open_review(const char *path, Experiment **out, char *error, size_t size)
{
    return exp_store_open_sidecar(path, 1, out, error, size);
}
int experiment_resume(const char *path, Experiment **out, char *error, size_t size)
{
    Experiment *e = NULL;
    char why[256];
    if (out)
        *out = NULL;
    if (!out || !exp_store_open_sidecar(path, 1, &e, error, size))
        return 0;
    if (e->replay || !monitor_source_pair_matches(e->source, e->pair)) {
        snprintf(e->error, sizeof(e->error),
                 "Source/evidence pairing unavailable; ordinary monitor DB is never resumed");
        goto failed;
    }
    if (!monitor_open(e->source, e->synthetic, &e->core, why, sizeof(why))) {
        snprintf(e->error, sizeof(e->error), "Source resume: %s", why);
        goto failed;
    }
    e->session = (uint64_t)monitor_source_session(e->core);
    if (!experiment_reset(e, monitor_utc_now(), "session restart; prior observation outcome unknown"))
        goto failed;
    *out = e;
    return 1;
failed:
    exp_store_copy_error(e, error, size);
    experiment_close(e);
    return 0;
}
int experiment_create(const char *source, const char *path, int synthetic, Experiment **out, char *error, size_t size)
{
    Experiment *e = calloc(1, sizeof(*e));
    sqlite3_stmt *s = NULL;
    char why[256];
    int rc;
    wchar_t a[32768], b[32768];
    if (out)
        *out = NULL;
    if (!e || !out) {
        free(e);
        return 0;
    }
    if (!exp_store_local_path(source, a) || !exp_store_local_path(path, b) || _wcsicmp(a, b) == 0 || !exp_store_file_absent(source) ||
        !exp_store_file_absent(path) || (synthetic != 0 && synthetic != 1)) {
        snprintf(e->error, sizeof(e->error), "Two distinct NEW local files required; existing DBs are preserved");
        goto failed;
    }
    e->source = exp_store_absolute_utf8(source);
    e->synthetic = synthetic;
    if (!e->source) {
        snprintf(e->error, sizeof(e->error), "Out of memory");
        goto failed;
    }
    if (sqlite3_open_v2(path, &e->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        exp_store_db_error(e, "create evidence");
        goto failed;
    }
    sqlite3_busy_timeout(e->db, 250);
    sqlite3_extended_result_codes(e->db, 1);
    if (!exp_store_execute(
            e,
            "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA trusted_schema=OFF; BEGIN IMMEDIATE") ||
        !exp_store_initialize_schema(e))
        goto failed;
    if (!exp_store_prepare(e, "SELECT lower(hex(randomblob(16)))", &s))
        goto failed;
    rc = sqlite3_step(s);
    if (rc != SQLITE_ROW || !(e->pair = exp_store_dup_text(exp_store_column_text(s, 0))))
        goto failed;
    sqlite3_finalize(s);
    s = NULL;
    if (!exp_store_prepare(e,
                 "INSERT INTO experiment VALUES(?,?,?,?,'" EXPERIMENT_TEMPLATE_VERSION
                 "',0,28,25,536870912,10485760,10485760)",
                 &s))
        goto failed;
    sqlite3_bind_text(s, 1, e->pair, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, e->source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 3, synthetic);
    sqlite3_bind_text(s, 4, EXPERIMENT_POLICY_VERSION, -1, SQLITE_STATIC);
    if (!exp_store_done(e, s))
        goto failed;
    sqlite3_finalize(s);
    s = NULL;
    if (!exp_store_execute(e, "COMMIT"))
        goto failed;
    if (!monitor_open(e->source, synthetic, &e->core, why, sizeof(why))) {
        snprintf(e->error, sizeof(e->error), "Source create: %s", why);
        goto failed;
    }
    if (!monitor_source_create_pair(e->core, e->pair)) {
        snprintf(e->error, sizeof(e->error), "Source pairing: %s", monitor_error(e->core));
        goto failed;
    }
    e->session = (uint64_t)monitor_source_session(e->core);
    *out = e;
    return 1;
failed:
    sqlite3_finalize(s);
    if (e->db)
        sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    exp_store_copy_error(e, error, size);
    experiment_close(e);
    return 0;
}

int experiment_reset(Experiment *e, uint64_t utc, const char *reason)
{
    if (!e || e->readonly || utc > INT64_MAX)
        return 0;
    if (!exp_store_execute(e, "BEGIN IMMEDIATE"))
        return 0;
    if (!exp_capture_gap(e, utc, 0, 0, 0, ALL_METRICS, reason ? reason : "explicit observation reset") || !exp_store_execute(e, "COMMIT")) {
        sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    free(e->tracks);
    e->tracks = NULL;
    e->track_count = 0;
    e->fault = 0;
    if (e->core)
        monitor_reset(e->core);
    return 1;
}
int experiment_finish(Experiment *e, uint64_t utc)
{
    return experiment_reset(e, utc, "capture stopped; later outcome unobserved");
}
void experiment_close(Experiment *e)
{
    if (!e)
        return;
    monitor_close(e->core);
    sqlite3_close(e->db);
    free(e->tracks);
    free(e->source);
    free(e->pair);
    free(e);
}

int experiment_ingest(Experiment *e, const ResourceBatch *batch, const char *const *paths, MonitorSnapshot **out)
{
    MonitorSnapshot *snapshot = NULL;
    Track *next_tracks = NULL;
    MonitorCommitRef commit_ref = {0};
    MonitorCommittedWindows source_windows = {0};
    if (out)
        *out = NULL;
    if (!e || !e->core || e->readonly || !out || !batch || (batch->count && !batch->items))
        return 0;
    if (e->fault) {
        snprintf(e->error, sizeof(e->error), "Evidence capture interrupted; explicit reset or resume required");
        return 0;
    }
    /* Track 준비 메모리는 core 호출 전에 확보해 이 할당 실패가 부분 저장을 만들지 않게 한다. */
    next_tracks = calloc(batch->count ? batch->count : 1, sizeof(*next_tracks));
    if (!next_tracks) {
        snprintf(e->error, sizeof(e->error), "Out of memory before core capture");
        return 0;
    }
    /* core 저장은 실험 근거 저장과 별도로 먼저 commit된다. */
    if (!monitor_ingest_with_commit_ref(e->core, batch, paths, &snapshot, &commit_ref)) {
        snprintf(e->error, sizeof(e->error), "Core capture failed: %s", monitor_error(e->core));
        e->fault = 1;
        goto failed;
    }
    if (!exp_store_execute(e, "BEGIN IMMEDIATE"))
        goto sidecar_failed;
    if (!exp_capture_absent_gaps(e, batch))
        goto sidecar_failed;
    if (!exp_capture_continuity(e, batch, next_tracks))
        goto sidecar_failed;
    /* source 조회와 배열 할당은 core commit 뒤에 수행한다. view는 살아 있는
       snapshot을 빌리며 capture가 즉시 값을 복사한다. 실패해도 core ingest를 재시도하지 않는다. */
    if (!monitor_source_read_committed(e->core, &commit_ref, snapshot, &source_windows)) {
        snprintf(e->error, sizeof(e->error), "Read just-committed source: %s", monitor_error(e->core));
        goto sidecar_failed;
    }
    if (!exp_capture_committed_windows(e, commit_ref.history_cutoff, &source_windows, next_tracks))
        goto sidecar_failed;
    monitor_source_windows_free(&source_windows);
    /* source view 해제 뒤 보관 정리를 적용하고 실험 DB를 commit한다. */
    if (!experiment_retention_trim(e->db, batch->finished_at_utc_100ns, e->error, sizeof(e->error)) ||
        !exp_store_execute(e, "COMMIT"))
        goto sidecar_failed;
    /* 실험 DB COMMIT이 성공해야 준비한 Track을 채택하고 snapshot 소유권을 호출자에게 넘긴다. */
    free(e->tracks);
    e->tracks = next_tracks;
    e->track_count = batch->count;
    *out = snapshot;
    return 1;
sidecar_failed:
    /* core commit은 남는다. 실험 DB만 rollback하고 reset 또는 resume 전까지 수집을 막는다. */
    monitor_source_windows_free(&source_windows);
    sqlite3_exec(e->db, "ROLLBACK", NULL, NULL, NULL);
    {
        char why[sizeof(e->error)];
        snprintf(why, sizeof(why), "%s", e->error);
        snprintf(e->error, sizeof(e->error), "Core committed; evidence capture FAILED: %.420s; reset required", why);
    }
    e->fault = 1;
    monitor_reset(e->core);
failed:
    free(next_tracks);
    monitor_snapshot_free(snapshot);
    return 0;
}

