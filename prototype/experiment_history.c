/* 보존된 실험 근거를 독립 소유 이력으로 읽고 평가·로컬 설명·export를 제공한다.
 * 최근 원본 회차 진단은 typed source 결과로 받아 과거 완료창 근거와 구분한다. */
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
#include "monitor_source.h"

static const char exp_history_decision_select[] =
    "SELECT d.id,d.window_id,w.source_window,w.source_run,w.cutoff,coalesce(d.episode_id,0),"
    "o.session,o.pid,o.creation,w.start,w.end,w.duration,w.name,w.path,d.metric,d.method,"
    "m.current,m.median,m.mad,d.threshold,m.covered,m.n,m.days,m.minutes,m.valid_samples,d.state,d.streak,m.eligible,m.reason,m.continuity,m.state "
    "FROM decision d JOIN window_evidence w ON w.id=d.window_id JOIN operation o ON o.id=w.id "
    "JOIN metric_evidence m ON m.window_id=w.id AND m.metric=d.metric ";
int exp_history_load_decision(sqlite3_stmt *s, ExperimentDecision *d)
{
    MonitorMetricReport *r = &d->report;
    memset(d, 0, sizeof(*d));
    d->id = sqlite3_column_int64(s, 0);
    d->window_id = sqlite3_column_int64(s, 1);
    d->source_window_id = sqlite3_column_int64(s, 2);
    d->source_run_id = sqlite3_column_int64(s, 3);
    d->source_cutoff = sqlite3_column_int64(s, 4);
    d->episode_id = sqlite3_column_int64(s, 5);
    d->session = (uint64_t)sqlite3_column_int64(s, 6);
    d->pid = (uint32_t)sqlite3_column_int64(s, 7);
    d->creation = (uint64_t)sqlite3_column_int64(s, 8);
    d->start_utc = (uint64_t)sqlite3_column_int64(s, 9);
    d->end_utc = (uint64_t)sqlite3_column_int64(s, 10);
    r->window_duration_seconds = sqlite3_column_double(s, 11);
    d->name = exp_store_dup_text(exp_store_column_text(s, 12));
    d->path = exp_store_dup_text(exp_store_column_text(s, 13));
    d->metric = (unsigned)sqlite3_column_int(s, 14);
    d->method = (ExperimentMethod)sqlite3_column_int(s, 15);
    r->current = exp_store_real_column(s, 16);
    r->median = exp_store_real_column(s, 17);
    r->mad = exp_store_real_column(s, 18);
    r->threshold = exp_store_real_column(s, 19);
    r->covered_seconds = sqlite3_column_double(s, 20);
    r->count = (unsigned)sqlite3_column_int(s, 21);
    r->days = (unsigned)sqlite3_column_int(s, 22);
    r->minutes = (unsigned)sqlite3_column_int(s, 23);
    r->valid_samples = (unsigned)sqlite3_column_int(s, 24);
    r->state = (MonitorState)sqlite3_column_int(s, 25);
    r->streak = (unsigned)sqlite3_column_int(s, 26);
    r->baseline_eligible = sqlite3_column_int(s, 27);
    snprintf(r->reason, sizeof(r->reason), "%s", exp_store_column_text(s, 28));
    d->continuity = sqlite3_column_int(s, 29);
    d->quality_valid = sqlite3_column_int(s, 30) != MONITOR_UNAVAILABLE && isfinite(r->current);
    if (d->method == EXPERIMENT_FIXED) {
        r->baseline_eligible = 0; /* 고정 규칙은 Baseline을 학습하지 않는다. */
        snprintf(r->reason, sizeof(r->reason), "%s",
                 r->state == MONITOR_UNAVAILABLE ? "shared observation quality insufficient"
                 : r->state == MONITOR_NORMAL    ? "at or below fixed upper limit; fixed rule has no training"
                                                 : "fixed upper limit exceeded; three consecutive windows required");
    }
    r->window_start_utc_100ns = d->start_utc;
    r->window_end_utc_100ns = d->end_utc;
    return d->name && d->path;
}
void experiment_decisions_free(ExperimentDecision *d, size_t n)
{
    size_t i;
    if (!d)
        return;
    for (i = 0; i < n; ++i) {
        free(d[i].name);
        free(d[i].path);
    }
    free(d);
}
static int decision_query(Experiment *e, const char *tail, int64_t arg1, int arg2, int arg3, ExperimentDecision **out,
                          size_t *count)
{
    char sql[2048];
    sqlite3_stmt *s = NULL;
    ExperimentDecision *rows;
    size_t n = 0, cap = arg2 > 0 ? (size_t)arg2 : 1;
    int rc;
    *out = NULL;
    *count = 0;
    rows = calloc(cap, sizeof(*rows));
    if (!rows) {
        snprintf(e->error, sizeof(e->error), "Out of memory reading decision copies");
        return 0;
    }
    snprintf(sql, sizeof(sql), "%s%s", exp_history_decision_select, tail);
    if (!exp_store_prepare(e, sql, &s)) {
        free(rows);
        return 0;
    }
    sqlite3_bind_int64(s, 1, arg1);
    sqlite3_bind_int(s, 2, arg2);
    sqlite3_bind_int(s, 3, arg3);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        if (n >= cap || !exp_history_load_decision(s, &rows[n])) {
            experiment_decisions_free(rows, n + (n < cap ? 1 : 0));
            sqlite3_finalize(s);
            snprintf(e->error, sizeof(e->error), "Decision copy capacity/allocation failed");
            return 0;
        }
        ++n;
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        exp_store_db_error(e, "decision list");
        experiment_decisions_free(rows, n);
        return 0;
    }
    *out = rows;
    *count = n;
    return 1;
}
int experiment_decisions(Experiment *e, ExperimentMethod method, unsigned limit, unsigned offset,
                         ExperimentDecision **out, size_t *count)
{
    if (!e || !out || !count || method < EXPERIMENT_PERSONALIZED || method > EXPERIMENT_FIXED || !limit ||
        limit > 10000 || offset > INT_MAX)
        return 0;
    return decision_query(e, "WHERE d.method=? ORDER BY d.id DESC LIMIT ? OFFSET ?", method, (int)limit, (int)offset,
                          out, count);
}
int experiment_get_decision(Experiment *e, int64_t id, ExperimentDecision **out)
{
    size_t n;
    if (!e || !out || id <= 0)
        return 0;
    if (!decision_query(e, "WHERE d.id=?", id, 1, 0, out, &n))
        return 0;
    if (n)
        return 1;
    experiment_decisions_free(*out, 0);
    *out = NULL;
    snprintf(e->error, sizeof(e->error), "Decision evidence unavailable or expired under 28-day retention");
    return 0;
}
void experiment_episodes_free(ExperimentEpisode *rows, size_t n)
{
    size_t i;
    if (!rows)
        return;
    for (i = 0; i < n; ++i) {
        free(rows[i].name);
        free(rows[i].path);
        free(rows[i].end_reason);
    }
    free(rows);
}
int experiment_episodes(Experiment *e, int method, unsigned limit, unsigned offset, ExperimentEpisode **out,
                        size_t *count)
{
    sqlite3_stmt *s = NULL;
    ExperimentEpisode *rows;
    size_t n = 0;
    int rc;
    if (!e || !out || !count || method < -1 || method > 1 || !limit || limit > 10000 || offset > INT_MAX)
        return 0;
    *out = NULL;
    *count = 0;
    rows = calloc(limit, sizeof(*rows));
    if (!rows)
        return 0;
    if (!exp_store_prepare(
            e,
            "SELECT id,activation_decision,last_decision,session,pid,creation,metric,method,state,first,activated,last,ended,qualifying,name,path,reason FROM episode WHERE (?<0 OR method=?) ORDER BY id DESC LIMIT ? OFFSET ?",
            &s)) {
        free(rows);
        return 0;
    }
    sqlite3_bind_int(s, 1, method);
    sqlite3_bind_int(s, 2, method);
    sqlite3_bind_int(s, 3, (int)limit);
    sqlite3_bind_int(s, 4, (int)offset);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        ExperimentEpisode *r = &rows[n];
        r->id = sqlite3_column_int64(s, 0);
        r->activation_decision_id = sqlite3_column_int64(s, 1);
        r->last_decision_id = sqlite3_column_int64(s, 2);
        r->session = (uint64_t)sqlite3_column_int64(s, 3);
        r->pid = (uint32_t)sqlite3_column_int64(s, 4);
        r->creation = (uint64_t)sqlite3_column_int64(s, 5);
        r->metric = (unsigned)sqlite3_column_int(s, 6);
        r->method = (ExperimentMethod)sqlite3_column_int(s, 7);
        r->state = (ExperimentEpisodeState)sqlite3_column_int(s, 8);
        r->first_utc = (uint64_t)sqlite3_column_int64(s, 9);
        r->activated_utc = (uint64_t)sqlite3_column_int64(s, 10);
        r->last_utc = (uint64_t)sqlite3_column_int64(s, 11);
        r->ended_utc = (uint64_t)sqlite3_column_int64(s, 12);
        r->qualifying_windows = (unsigned)sqlite3_column_int(s, 13);
        r->name = exp_store_dup_text(exp_store_column_text(s, 14));
        r->path = exp_store_dup_text(exp_store_column_text(s, 15));
        r->end_reason = exp_store_dup_text(exp_store_column_text(s, 16));
        ++n;
        if (!r->name || !r->path || !r->end_reason) {
            sqlite3_finalize(s);
            experiment_episodes_free(rows, n);
            return 0;
        }
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        exp_store_db_error(e, "episode list");
        experiment_episodes_free(rows, n);
        return 0;
    }
    *out = rows;
    *count = n;
    return 1;
}
/* 원본 짝 확인과 최근 raw 조회 완전성은 별개다. 일부 진단 값이 있어도
 * latest_source_available이 거짓이면 결측 수를 완전한 집계로 해석하지 않는다. */
static void source_diagnostics(Experiment *e, ExperimentStats *out)
{
    MonitorSourceDiagnostics source = {0};
    unsigned m;
    if (e->replay)
        return;
    monitor_source_diagnostics(e->source, e->pair, &source);
    out->source_available = source.source_available;
    out->latest_source_available = source.available;
    out->latest_source_utc = source.utc;
    out->latest_source_status = source.status;
    out->latest_source_rows = source.rows;
    out->latest_source_raw_rows = source.raw_rows;
    out->latest_source_identity_unavailable = source.identity_unavailable;
    out->latest_source_path_unavailable = source.path_unavailable;
    for (m = 0; m < MONITOR_METRICS; ++m)
        out->latest_source_metric_missing[m] = source.metric_missing[m];
}
int experiment_stats(Experiment *e, ExperimentStats *out)
{
    sqlite3_stmt *s = NULL;
    int rc;
    if (!e || !out)
        return 0;
    memset(out, 0, sizeof(*out));
    out->replay_only = e->replay;
    out->synthetic = e->synthetic;
    out->replay_comparator_recomputed = e->replay && !e->expired_prefix;
    source_diagnostics(e, out);
    if (!exp_store_prepare(
            e,
            "SELECT (SELECT count(*) FROM window_evidence),(SELECT count(*) FROM decision),(SELECT count(*) FROM episode),"
            "(SELECT count(*) FROM episode WHERE state=0),(SELECT count(*) FROM episode WHERE state=1),(SELECT count(*) FROM episode WHERE state=2),"
            "(SELECT count(*) FROM evaluation),(SELECT count(*) FROM operation WHERE kind=1)",
            &s))
        return 0;
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        out->windows = (uint64_t)sqlite3_column_int64(s, 0);
        out->decisions = (uint64_t)sqlite3_column_int64(s, 1);
        out->episodes = (uint64_t)sqlite3_column_int64(s, 2);
        out->active = (uint64_t)sqlite3_column_int64(s, 3);
        out->recovered = (uint64_t)sqlite3_column_int64(s, 4);
        out->unknown = (uint64_t)sqlite3_column_int64(s, 5);
        out->evaluations = (uint64_t)sqlite3_column_int64(s, 6);
        out->gaps = (uint64_t)sqlite3_column_int64(s, 7);
    }
    sqlite3_finalize(s);
    s = NULL;
    if (rc != SQLITE_ROW)
        return 0;
    if (!exp_store_prepare(
            e,
            "SELECT d.method,d.state,count(*),sum(w.duration) FROM decision d JOIN window_evidence w ON w.id=d.window_id GROUP BY d.method,d.state",
            &s))
        return 0;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        int method = sqlite3_column_int(s, 0), state = sqlite3_column_int(s, 1);
        if (method >= 0 && method < 2 && state >= 0 && state < 5) {
            out->states[method][state] = (uint64_t)sqlite3_column_int64(s, 2);
            out->window_seconds[method][state] = sqlite3_column_double(s, 3);
        }
    }
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}
static int ternary(int v)
{
    return v >= -1 && v <= 1;
}
int experiment_evaluate(Experiment *e, const ExperimentEvaluation *v)
{
    sqlite3_stmt *s = NULL;
    int ok;
    if (!e || e->readonly || !v || v->episode_id < 0 || v->decision_id < 0 || (!v->episode_id && !v->decision_id) ||
        !v->trial_id || !v->trial_id[0] || !v->order_id || v->judgment < EXPERIMENT_UNRATED ||
        v->judgment > EXPERIMENT_UNSURE || !ternary(v->understood) || !ternary(v->target_correct) ||
        !ternary(v->time_correct) || !ternary(v->metric_correct) || !ternary(v->next_check) ||
        !isfinite(v->elapsed_seconds)) {
        if (e)
            snprintf(
                e->error, sizeof(e->error),
                "Evaluation requires an existing target, nonempty trial ID, order ID, valid unknown/no/yes fields and finite optional elapsed time");
        return 0;
    }
    if (!exp_store_prepare(
            e,
            "SELECT (?=0 OR EXISTS(SELECT 1 FROM episode WHERE id=?)) AND (?=0 OR EXISTS(SELECT 1 FROM decision WHERE id=?)) AND "
            "(?=0 OR ?=0 OR EXISTS(SELECT 1 FROM decision WHERE id=? AND episode_id=?))",
            &s))
        return 0;
    sqlite3_bind_int64(s, 1, v->episode_id);
    sqlite3_bind_int64(s, 2, v->episode_id);
    sqlite3_bind_int64(s, 3, v->decision_id);
    sqlite3_bind_int64(s, 4, v->decision_id);
    sqlite3_bind_int64(s, 5, v->episode_id);
    sqlite3_bind_int64(s, 6, v->decision_id);
    sqlite3_bind_int64(s, 7, v->decision_id);
    sqlite3_bind_int64(s, 8, v->episode_id);
    ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    s = NULL;
    if (!ok) {
        snprintf(e->error, sizeof(e->error), "Evaluation target unavailable, expired or inconsistent");
        return 0;
    }
    if (!exp_store_prepare(
            e,
            "INSERT INTO evaluation(episode,decision,trial,ordering,judgment,understood,target_correct,time_correct,metric_correct,next_check,elapsed,note,created) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            &s))
        return 0;
    if (v->episode_id)
        sqlite3_bind_int64(s, 1, v->episode_id);
    if (v->decision_id)
        sqlite3_bind_int64(s, 2, v->decision_id);
    sqlite3_bind_text(s, 3, v->trial_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, v->order_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 5, v->judgment);
    sqlite3_bind_int(s, 6, v->understood);
    sqlite3_bind_int(s, 7, v->target_correct);
    sqlite3_bind_int(s, 8, v->time_correct);
    sqlite3_bind_int(s, 9, v->metric_correct);
    sqlite3_bind_int(s, 10, v->next_check);
    if (v->elapsed_seconds >= 0)
        sqlite3_bind_double(s, 11, v->elapsed_seconds);
    sqlite3_bind_text(s, 12, v->note ? v->note : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 13, (int64_t)monitor_utc_now());
    ok = exp_store_done(e, s);
    sqlite3_finalize(s);
    return ok;
}
static const char *utc_text(uint64_t utc, char *text, size_t size)
{
    FILETIME ft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)utc;
    ft.dwHighDateTime = (DWORD)(utc >> 32);
    if (!utc || !FileTimeToSystemTime(&ft, &st))
        snprintf(text, size, "unavailable UTC");
    else
        snprintf(text, size, "%04u-%02u-%02u %02u:%02u:%02u.%03u UTC", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond, st.wMilliseconds);
    return text;
}
static void display_metadata(const char *source, char *out, size_t size)
{
    static const char suffix[] = " [truncated; full metadata in export]";
    size_t n;
    if (!source)
        source = "unavailable";
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1, NULL, 0)) {
        snprintf(out, size, "invalid UTF-8 metadata; original bytes retained in export");
        return;
    }
    n = strlen(source);
    if (n < size) {
        memcpy(out, source, n + 1);
        return;
    }
    n = size > sizeof(suffix) ? size - sizeof(suffix) : 0;
    while (n && (((unsigned char)source[n] & 0xc0) == 0x80))
        --n;
    memcpy(out, source, n);
    memcpy(out + n, suffix, sizeof(suffix));
}
int experiment_explain(const ExperimentDecision *d, uint64_t now, char *text, size_t size)
{
    const MonitorMetricReport *r;
    char fact[512], comparison[768], freshness[128], start[64], end[64], creation[64], delta[192], history[256];
    char name[192], path[384];
    const char *unit;
    int written;
    if (!d || !text || !size || d->metric >= 4)
        return 0;
    r = &d->report;
    display_metadata(d->name, name, sizeof(name));
    display_metadata(d->path, path, sizeof(path));
    unit = d->metric == 0   ? "% of total logical CPU capacity"
           : d->metric == 1 ? "WorkingSet bytes (may include shared pages)"
                            : "process B/s (not physical disk utilization)";
    if (!isfinite(r->current))
        snprintf(fact, sizeof(fact), "Observation: completed window has no valid %s value.",
                 monitor_metric_name(d->metric));
    else
        snprintf(fact, sizeof(fact),
                 "Observation: completed %.3f-second window averaged %.6g %s; %u valid samples; %.3f covered seconds.",
                 r->window_duration_seconds, r->current, unit, r->valid_samples, r->covered_seconds);
    if (isfinite(r->median))
        snprintf(
            history, sizeof(history),
            "Historical median %.6g (%u representatives / %u UTC dates / %u minutes) is context, not this rule's limit.",
            r->median, r->count, r->days, r->minutes);
    else
        snprintf(history, sizeof(history),
                 "Historical median is unavailable; this fixed rule does not require baseline training.");
    if (r->state == MONITOR_UNAVAILABLE)
        snprintf(comparison, sizeof(comparison),
                 "Comparison unavailable: metric quality is insufficient; missing values are not zero.");
    else if (r->state == MONITOR_LEARNING)
        snprintf(
            comparison, sizeof(comparison),
            "Comparison not ready: %u representatives / %u UTC dates / %u distinct minutes; requires 60 / 3 / 60. No personalized normality judgment.",
            r->count, r->days, r->minutes);
    else if (d->method == EXPERIMENT_FIXED)
        snprintf(
            comparison, sizeof(comparison),
            "Fixed policy upper limit %.6g; state %s; %u consecutive exceeding windows. Same observation window and quality as personalized. %s",
            r->threshold, monitor_state_name(r->state), r->streak, history);
    else
        snprintf(
            comparison, sizeof(comparison),
            "Same exact-path individual-instance history: median %.6g, MAD %.6g, upper limit %.6g; %u representatives / %u UTC dates / %u distinct minutes. State %s; %u consecutive exceeding windows. Values were compared before learning.",
            r->median, r->mad, r->threshold, r->count, r->days, r->minutes, monitor_state_name(r->state), r->streak);
    if (now > d->end_utc && now - d->end_utc > 90 * TICKS)
        snprintf(freshness, sizeof(freshness),
                 "Historical/stale completed window; current process state is unconfirmed.");
    else
        snprintf(freshness, sizeof(freshness), "Completed-window evidence; distinct from latest raw observation.");
    if (isfinite(r->current) && isfinite(r->median) && isfinite(r->threshold))
        snprintf(
            delta, sizeof(delta),
            "Absolute difference: current minus historical median %.6g; current minus method limit %.6g (same metric units; no ratio).",
            r->current - r->median, r->current - r->threshold);
    else if (isfinite(r->current) && isfinite(r->threshold))
        snprintf(
            delta, sizeof(delta),
            "Absolute difference: current minus method limit %.6g; historical difference unavailable (same metric units; no ratio).",
            r->current - r->threshold);
    else
        snprintf(delta, sizeof(delta),
                 "Absolute difference unavailable where historical or comparison evidence is missing.");
    written = snprintf(
        text, size,
        "Target: %s; PID %lu; creation %s; exact path %s.\r\nWindow: %s to %s.\r\n%s\r\n%s\r\n%s\r\n%s\r\nUnknown: these observations do not establish the cause, user need, malware status, memory leak or physical disk bottleneck.\r\nNext check: inspect the app's current task and the change time, compare the other resource metrics, then observe again after the task completes.\r\nPolicy %s; template %s; source session %llu/run %lld/window %lld; cutoff %lld; partial-sample continuity %s.\r\nRaw source retention is 1 hour; preserved comparison summaries do not guarantee original baseline samples remain available for refitting.",
        name, (unsigned long)d->pid, utc_text(d->creation, creation, sizeof(creation)), path,
        utc_text(d->start_utc, start, sizeof(start)), utc_text(d->end_utc, end, sizeof(end)), fact, comparison, delta,
        freshness, EXPERIMENT_POLICY_VERSION, EXPERIMENT_TEMPLATE_VERSION, (unsigned long long)d->session,
        (long long)d->source_run_id, (long long)d->source_window_id, (long long)d->source_cutoff,
        d->continuity ? "preserved" : "interrupted");
    if (written < 0 || (size_t)written >= size) {
        snprintf(text, size, "Explanation buffer too small");
        return 0;
    }
    return 1;
}

static int csv_cell(FILE *f, const char *p)
{
    const unsigned char *s = (const unsigned char *)(p ? p : "");
    if (fputc('"', f) == EOF)
        return 0;
    while (*s) {
        if (*s == '"' && fputc('"', f) == EOF)
            return 0;
        if (fputc(*s++, f) == EOF)
            return 0;
    }
    return fputc('"', f) != EOF;
}
int experiment_export(Experiment *e, const char *path)
{
    static const char query[] =
        "SELECT 'metadata',0,json_object('origin',pair,'source_path',source,'synthetic',synthetic,'policy',policy,'template',template,'replay',replay,'retention_days',retention_days,'fixed_cpu_percent',cpu_limit,'fixed_workingset_bytes',ram_limit,'fixed_read_Bps',read_limit,'fixed_write_Bps',write_limit,'replay_semantics','recorded comparator decisions, not baseline refit; expired activation references may be unavailable') FROM experiment "
        "UNION ALL SELECT 'replay_info',0,json_object('mode',mode) FROM replay_info "
        "UNION ALL SELECT 'retention_state',0,json_object('expired_prefix',expired_prefix) FROM retention_state "
        "UNION ALL SELECT 'operation',id,json_object('kind',kind,'utc',utc,'session',session,'pid',pid,'creation',creation,'metric_mask',mask,'reason',reason) FROM operation "
        "UNION ALL SELECT 'window',id,json_object('source_window',source_window,'source_run',source_run,'cutoff',cutoff,'start_utc',start,'end_utc',end,'duration_seconds',duration,'name',name,'path',path,'suppression_kind',feedback,'suppression_expires',feedback_expires) FROM window_evidence "
        "UNION ALL SELECT 'metric_evidence',window_id,json_object('metric',metric,'current',current,'personal_median',median,'personal_MAD',mad,'personal_threshold',threshold,'covered_seconds',covered,'representatives',n,'UTC_dates',days,'minutes',minutes,'valid_samples',valid_samples,'personal_state',state,'personal_streak',streak,'personal_eligible',eligible,'personal_reason',reason,'continuity',continuity) FROM metric_evidence "
        "UNION ALL SELECT 'decision',id,json_object('window_id',window_id,'metric',metric,'method',method,'state',state,'streak',streak,'threshold',threshold,'episode',episode_id) FROM decision "
        "UNION ALL SELECT 'episode',id,json_object('session',session,'pid',pid,'creation',creation,'metric',metric,'method',method,'state',state,'first_observed_start',first,'activated',activated,'last_exceed_end',last,'ended',ended,'activation_decision',activation_decision,'latest_or_recovery_decision',last_decision,'qualifying_windows',qualifying,'name',name,'path',path,'reason',reason) FROM episode "
        "UNION ALL SELECT 'evaluation',id,json_object('episode',episode,'decision',decision,'trial',trial,'order',ordering,'judgment',judgment,'understood',understood,'target_correct',target_correct,'time_correct',time_correct,'metric_correct',metric_correct,'next_check',next_check,'elapsed_seconds',elapsed,'note',note,'created',created) FROM evaluation";
    wchar_t full[32768];
    HANDLE h;
    FILE *f;
    sqlite3_stmt *s = NULL;
    int fd, rc, ok = 1;
    if (!e || !exp_store_local_path(path, full))
        return 0;
    h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(e->error, sizeof(e->error), "Export requires a NEW local file (Win32 %lu)",
                 (unsigned long)GetLastError());
        return 0;
    }
    /* 성공하면 HANDLE 소유권이 fd로, 이어 FILE로 넘어가 fclose가 정리한다. */
    fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return 0;
    }
    f = _fdopen(fd, "wb");
    if (!f) {
        _close(fd);
        return 0;
    }
    if (fputs("record_type,record_id,payload_json\r\n", f) == EOF || !exp_store_prepare(e, query, &s))
        ok = 0;
    if (ok) {
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            if (!csv_cell(f, exp_store_column_text(s, 0)) || fputc(',', f) == EOF ||
                fprintf(f, "%lld,", (long long)sqlite3_column_int64(s, 1)) < 0 || !csv_cell(f, exp_store_column_text(s, 2)) ||
                fputs("\r\n", f) == EOF) {
                ok = 0;
                break;
            }
        }
        if (rc != SQLITE_DONE) {
            exp_store_db_error(e, "export query/write");
            ok = 0;
        }
    }
    sqlite3_finalize(s);
    if (fclose(f) != 0)
        ok = 0;
    if (!ok)
        snprintf(e->error, sizeof(e->error), "Export failed; new output may be incomplete");
    return ok;
}


int exp_history_prepare_replay(Experiment *e, sqlite3_stmt **s)
{
    char sql[2048];
    snprintf(sql, sizeof(sql), "%sWHERE d.window_id=? ORDER BY d.metric,d.method", exp_history_decision_select);
    return exp_store_prepare(e, sql, s);
}
