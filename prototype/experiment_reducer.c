/* 같은 실행 인스턴스·지표·비교 방식별 연속 초과와 사건 상태를 실험 DB에 반영한다.
 * 관측 단절은 회복이 아니며 core의 학습이나 수집 상태를 변경하지 않는다. */
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "experiment_internal.h"

int exp_reduce_gap(Experiment *e, uint64_t utc, uint64_t session, uint32_t pid, uint64_t creation, unsigned mask,
                      const char *reason)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!exp_store_prepare(
            e,
            "UPDATE episode SET state=2,ended=?,reason=? WHERE state=0 AND (?=0 OR session=?) AND (?=0 OR (pid=? AND creation=?)) AND ((1<<metric)&?)<>0",
            &s))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)utc);
    sqlite3_bind_text(s, 2, reason ? reason : "observation break", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, (int64_t)session);
    sqlite3_bind_int64(s, 4, (int64_t)session);
    sqlite3_bind_int64(s, 5, pid);
    sqlite3_bind_int64(s, 6, pid);
    sqlite3_bind_int64(s, 7, (int64_t)creation);
    sqlite3_bind_int(s, 8, (int)mask);
    ok = exp_store_done(e, s);
    sqlite3_finalize(s);
    s = NULL;
    if (!ok)
        return 0;
    if (!exp_store_prepare(
            e,
            "DELETE FROM reduction WHERE (?=0 OR session=?) AND (?=0 OR (pid=? AND creation=?)) AND ((1<<metric)&?)<>0",
            &s))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)session);
    sqlite3_bind_int64(s, 2, (int64_t)session);
    sqlite3_bind_int64(s, 3, pid);
    sqlite3_bind_int64(s, 4, pid);
    sqlite3_bind_int64(s, 5, (int64_t)creation);
    sqlite3_bind_int(s, 6, (int)mask);
    ok = exp_store_done(e, s);
    sqlite3_finalize(s);
    return ok;
}
/* 복사해 보존한 보고서와 순서 있는 관측 단절만으로 판정·사건 상태를 갱신한다. */
int exp_reduce_decision(Experiment *e, int64_t window_id, uint64_t session, uint32_t pid, uint64_t creation, const char *name,
                  const char *path, unsigned metric, const MonitorMetricReport *report, ExperimentMethod method)
{
    sqlite3_stmt *s = NULL;
    int rc, prior_streak = 0, state, streak;
    int64_t first_window_utc = (int64_t)report->window_start_utc_100ns, episode_id = 0, decision_id;
    double threshold = method == EXPERIMENT_FIXED ? exp_fixed_limits[metric] : report->threshold;
    /* 실행 인스턴스·지표·비교 방식이 모두 같은 이전 연속 상태를 읽는다. */
    if (!exp_store_prepare(
            e,
            "SELECT streak,first,episode FROM reduction WHERE session=? AND pid=? AND creation=? AND metric=? AND method=?",
            &s))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)session);
    sqlite3_bind_int64(s, 2, pid);
    sqlite3_bind_int64(s, 3, (int64_t)creation);
    sqlite3_bind_int(s, 4, (int)metric);
    sqlite3_bind_int(s, 5, method);
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        prior_streak = sqlite3_column_int(s, 0);
        first_window_utc = sqlite3_column_int64(s, 1);
        episode_id = sqlite3_column_int64(s, 2);
    } else if (rc != SQLITE_DONE) {
        exp_store_db_error(e, "reduction read");
        sqlite3_finalize(s);
        return 0;
    }
    sqlite3_finalize(s);
    s = NULL;
    /* replay에서는 활성화 근거가 만료되고 경계 사건 요약만 남아 있을 수 있다. */
    if (!episode_id && e->replayed_episode) {
        if (!exp_store_prepare(e, "SELECT id FROM episode WHERE id=? AND state=0", &s))
            return 0;
        sqlite3_bind_int64(s, 1, e->replayed_episode->id);
        rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) {
            episode_id = sqlite3_column_int64(s, 0);
            first_window_utc = (int64_t)e->replayed_episode->first_utc;
        } else if (rc != SQLITE_DONE) {
            sqlite3_finalize(s);
            return 0;
        }
        sqlite3_finalize(s);
        s = NULL;
    }
    /* 비교 결과를 정한다. 앞부분이 온전한 고정 규칙 replay는 재계산해 원래 결정과 대조한다. */
    if (e->replayed_decision && (method == EXPERIMENT_PERSONALIZED || e->expired_prefix)) {
        state = e->replayed_decision->report.state;
        streak = (int)e->replayed_decision->report.streak;
        threshold = e->replayed_decision->report.threshold;
    } else if (method == EXPERIMENT_PERSONALIZED) {
        state = report->state;
        streak = (int)report->streak;
    } else if (report->state == MONITOR_UNAVAILABLE || !isfinite(report->current)) {
        state = MONITOR_UNAVAILABLE;
        streak = 0;
    } else if (report->current > threshold) {
        streak = prior_streak == INT_MAX ? INT_MAX : prior_streak + 1;
        state = streak >= 3 ? MONITOR_CANDIDATE : MONITOR_ATTENTION;
    } else {
        state = MONITOR_NORMAL;
        streak = 0;
    }
    if (e->replayed_decision && method == EXPERIMENT_FIXED && !e->expired_prefix &&
        (state != e->replayed_decision->report.state || streak != (int)e->replayed_decision->report.streak ||
         threshold != e->replayed_decision->report.threshold)) {
        snprintf(e->error, sizeof(e->error), "Fixed comparator replay mismatch at decision %lld; replay refused",
                 (long long)e->replayed_decision->id);
        return 0;
    }
    if (!prior_streak && streak)
        first_window_utc = (int64_t)report->window_start_utc_100ns;
    /* 사건 상태를 바꾸기 전에 이번 창의 비교 결정을 저장한다. */
    if (!exp_store_prepare(e, "INSERT INTO decision(id,window_id,metric,method,state,streak,threshold) VALUES(?,?,?,?,?,?,?)",
                 &s))
        return 0;
    if (e->replayed_decision)
        sqlite3_bind_int64(s, 1, e->replayed_decision->id);
    sqlite3_bind_int64(s, 2, window_id);
    sqlite3_bind_int(s, 3, (int)metric);
    sqlite3_bind_int(s, 4, method);
    sqlite3_bind_int(s, 5, state);
    sqlite3_bind_int(s, 6, streak);
    exp_store_bind_real(s, 7, threshold);
    if (!exp_store_done(e, s)) {
        sqlite3_finalize(s);
        return 0;
    }
    sqlite3_finalize(s);
    s = NULL;
    decision_id = sqlite3_last_insert_rowid(e->db);
    /* 후보면 사건을 생성·갱신한다. 기존 사건은 유효한 정상창에서만 회복으로 종료한다. */
    if (state == MONITOR_CANDIDATE) {
        if (!episode_id) {
            const ExperimentEpisode *seed = e->replayed_episode;
            if (!exp_store_prepare(
                    e,
                    "INSERT INTO episode(id,session,pid,creation,metric,method,state,first,activated,last,ended,activation_decision,last_decision,qualifying,name,path,reason) VALUES(?,?,?,?,?,?,0,?,?,?,0,?,?,?,?,?,'')",
                    &s))
                return 0;
            if (seed)
                sqlite3_bind_int64(s, 1, seed->id);
            sqlite3_bind_int64(s, 2, (int64_t)session);
            sqlite3_bind_int64(s, 3, pid);
            sqlite3_bind_int64(s, 4, (int64_t)creation);
            sqlite3_bind_int(s, 5, (int)metric);
            sqlite3_bind_int(s, 6, method);
            sqlite3_bind_int64(s, 7, seed ? (int64_t)seed->first_utc : first_window_utc);
            sqlite3_bind_int64(s, 8, seed ? (int64_t)seed->activated_utc : (int64_t)report->window_end_utc_100ns);
            sqlite3_bind_int64(s, 9, (int64_t)report->window_end_utc_100ns);
            sqlite3_bind_int64(s, 10, seed ? seed->activation_decision_id : decision_id);
            sqlite3_bind_int64(s, 11, decision_id);
            sqlite3_bind_int(s, 12, streak);
            sqlite3_bind_text(s, 13, name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 14, path, -1, SQLITE_TRANSIENT);
            if (!exp_store_done(e, s)) {
                sqlite3_finalize(s);
                return 0;
            }
            episode_id = sqlite3_last_insert_rowid(e->db);
            sqlite3_finalize(s);
            s = NULL;
        } else {
            if (!exp_store_prepare(e, "UPDATE episode SET last=?,last_decision=?,qualifying=? WHERE id=?", &s))
                return 0;
            sqlite3_bind_int64(s, 1, (int64_t)report->window_end_utc_100ns);
            sqlite3_bind_int64(s, 2, decision_id);
            sqlite3_bind_int(s, 3, streak);
            sqlite3_bind_int64(s, 4, episode_id);
            if (!exp_store_done(e, s)) {
                sqlite3_finalize(s);
                return 0;
            }
            sqlite3_finalize(s);
            s = NULL;
        }
    } else if (episode_id) {
        int recovered = state == MONITOR_NORMAL;
        if (!exp_store_prepare(e, "UPDATE episode SET state=?,ended=?,reason=?,last_decision=? WHERE id=?", &s))
            return 0;
        sqlite3_bind_int(s, 1, recovered ? EXPERIMENT_RECOVERED : EXPERIMENT_UNKNOWN);
        sqlite3_bind_int64(s, 2, (int64_t)report->window_end_utc_100ns);
        sqlite3_bind_text(s, 3,
                          recovered ? "observed valid window at or below policy threshold"
                                    : "learning or unavailable comparison; recovery unobserved",
                          -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 4, decision_id);
        sqlite3_bind_int64(s, 5, episode_id);
        if (!exp_store_done(e, s)) {
            sqlite3_finalize(s);
            return 0;
        }
        sqlite3_finalize(s);
        s = NULL;
    }
    /* 이번 창에서 사건이 끝났어도 종료 근거인 결정과 사건의 연결을 유지한다. */
    if (episode_id) {
        if (!exp_store_prepare(e, "UPDATE decision SET episode_id=? WHERE id=?", &s))
            return 0;
        sqlite3_bind_int64(s, 1, episode_id);
        sqlite3_bind_int64(s, 2, decision_id);
        if (!exp_store_done(e, s)) {
            sqlite3_finalize(s);
            return 0;
        }
        sqlite3_finalize(s);
        s = NULL;
    }
    /* 다음 창의 연속 상태에는 아직 활성 상태인 사건 ID만 남긴다. */
    if (state != MONITOR_CANDIDATE)
        episode_id = 0;
    if (!exp_store_prepare(e, "INSERT OR REPLACE INTO reduction VALUES(?,?,?,?,?,?,?,?)", &s))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)session);
    sqlite3_bind_int64(s, 2, pid);
    sqlite3_bind_int64(s, 3, (int64_t)creation);
    sqlite3_bind_int(s, 4, (int)metric);
    sqlite3_bind_int(s, 5, method);
    sqlite3_bind_int(s, 6, streak);
    sqlite3_bind_int64(s, 7, first_window_utc);
    sqlite3_bind_int64(s, 8, episode_id);
    rc = exp_store_done(e, s);
    sqlite3_finalize(s);
    return rc;
}
