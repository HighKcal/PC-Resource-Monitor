/* 원본 실험 DB를 읽기 전용으로 열어 새 DB에 복사한 뒤 기록 순서대로 사건을 재생한다.
 * 앞부분 만료 여부에 따라 고정 규칙 재계산 대조와 기록된 결정의 재구성을 구분한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "experiment_internal.h"

static int replay_episode_seed(Experiment *source, int64_t episode_id, ExperimentEpisode *out)
{
    sqlite3_stmt *s = NULL;
    int rc;
    memset(out, 0, sizeof(*out));
    if (!episode_id)
        return 1;
    if (!exp_store_prepare(source, "SELECT id,first,activated,activation_decision FROM episode WHERE id=?", &s))
        return 0;
    sqlite3_bind_int64(s, 1, episode_id);
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        out->id = sqlite3_column_int64(s, 0);
        out->first_utc = (uint64_t)sqlite3_column_int64(s, 1);
        out->activated_utc = (uint64_t)sqlite3_column_int64(s, 2);
        out->activation_decision_id = sqlite3_column_int64(s, 3);
    }
    sqlite3_finalize(s);
    return rc == SQLITE_ROW;
}
int experiment_replay(const char *path, const char *new_path, char *error, size_t size)
{
    Experiment *source = NULL, *dest = NULL;
    sqlite3_backup *backup = NULL;
    sqlite3_stmt *operations = NULL, *decisions = NULL;
    int rc, ok = 0, created = 0;
    if (error && size)
        error[0] = 0;
    if (!exp_store_file_absent(new_path)) {
        if (error && size)
            snprintf(error, size, "Replay requires a NEW local destination; files preserved");
        return 0;
    }
    if (!experiment_open_history(path, &source, error, size))
        return 0;
    dest = calloc(1, sizeof(*dest));
    if (!dest)
        goto failed;
    dest->expired_prefix = source->expired_prefix;
    if (sqlite3_open_v2(new_path, &dest->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        exp_store_db_error(dest, "create replay");
        goto failed;
    }
    created = 1;
    sqlite3_busy_timeout(dest->db, 250);
    if (!exp_store_execute(source, "BEGIN"))
        goto failed;
    backup = sqlite3_backup_init(dest->db, "main", source->db, "main");
    if (!backup) {
        exp_store_db_error(dest, "replay backup init");
        goto failed;
    }
    rc = sqlite3_backup_step(backup, -1);
    if (sqlite3_backup_finish(backup) != SQLITE_OK || rc != SQLITE_DONE) {
        backup = NULL;
        exp_store_db_error(dest, "replay evidence copy");
        goto failed;
    }
    backup = NULL;
    if (!exp_store_execute(
            dest,
            "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA trusted_schema=OFF; BEGIN IMMEDIATE;"
            "DROP TABLE IF EXISTS replay_boundary; CREATE TABLE replay_boundary AS SELECT * FROM episode WHERE activation_decision NOT IN(SELECT id FROM decision);"
            "DELETE FROM reduction; DELETE FROM decision; DELETE FROM episode;"
            "INSERT INTO episode SELECT * FROM replay_boundary; UPDATE episode SET state=0,ended=0,reason='retained boundary summary; activation evidence expired';"
            "UPDATE experiment SET source='',replay=1;"
            "UPDATE replay_info SET mode=CASE WHEN (SELECT expired_prefix FROM retention_state)=0 THEN 'fixed-comparator-recomputed-and-verified' ELSE 'projection-only-expired-prefix' END;"))
        goto failed;
    if (!exp_store_prepare(source, "SELECT id,kind,utc,session,pid,creation,mask,reason FROM operation ORDER BY id", &operations))
        goto failed;
    if (!exp_history_prepare_replay(source, &decisions))
        goto failed;
    while ((rc = sqlite3_step(operations)) == SQLITE_ROW) {
        uint64_t utc = (uint64_t)sqlite3_column_int64(operations, 2),
                 session = (uint64_t)sqlite3_column_int64(operations, 3);
        uint32_t pid = (uint32_t)sqlite3_column_int64(operations, 4);
        uint64_t creation = (uint64_t)sqlite3_column_int64(operations, 5);
        if (sqlite3_column_int(operations, 1) == 1) {
            if (!exp_reduce_gap(dest, utc, session, pid, creation, (unsigned)sqlite3_column_int(operations, 6),
                            exp_store_column_text(operations, 7)))
                goto failed;
        } else {
            int drc;
            sqlite3_bind_int64(decisions, 1, sqlite3_column_int64(operations, 0));
            while ((drc = sqlite3_step(decisions)) == SQLITE_ROW) {
                ExperimentDecision d;
                ExperimentEpisode seed;
                MonitorMetricReport observation;
                if (!exp_history_load_decision(decisions, &d)) {
                    free(d.name);
                    free(d.path);
                    goto failed;
                }
                if (!replay_episode_seed(source, d.episode_id, &seed)) {
                    free(d.name);
                    free(d.path);
                    goto failed;
                }
                /* 현재 결정의 지역 복사본만 동기 호출 동안 빌리고 반환 직후 연결을 지운다. */
                dest->replayed_decision = &d;
                dest->replayed_episode = seed.id ? &seed : NULL;
                observation = d.report;
                if (d.method == EXPERIMENT_FIXED)
                    observation.state = d.quality_valid ? MONITOR_NORMAL : MONITOR_UNAVAILABLE;
                ok = exp_reduce_decision(dest, d.window_id, d.session, d.pid, d.creation, d.name, d.path, d.metric, &observation,
                            d.method);
                dest->replayed_decision = NULL;
                dest->replayed_episode = NULL;
                free(d.name);
                free(d.path);
                if (!ok)
                    goto failed;
            }
            sqlite3_reset(decisions);
            sqlite3_clear_bindings(decisions);
            if (drc != SQLITE_DONE)
                goto failed;
        }
    }
    /* 개수 제한 보관 정리로 결정은 모두 만료되고 사건 요약만 남을 수 있다.
       이 행은 관측을 재구성한 결과가 아닌 기록된 요약이다. 더 이른 전역 단절이
       최종 상태를 바꾸지 않도록 다른 재생을 마친 뒤 복원한다. */
    if (rc != SQLITE_DONE ||
        !exp_store_execute(dest, "INSERT OR REPLACE INTO episode SELECT * FROM replay_boundary WHERE id NOT IN(SELECT episode_id FROM decision WHERE episode_id IS NOT NULL)") ||
        !exp_store_execute(dest, "COMMIT") || !exp_store_execute(source, "COMMIT"))
        goto failed;
    ok = 1;
    goto finished;
failed:
    ok = 0;
    if (error && size)
        snprintf(error, size, "Replay failed: %s",
                 dest && dest->error[0]       ? dest->error
                 : source && source->error[0] ? source->error
                                              : "allocation or incomplete evidence; output is not a completed replay");
    if (dest && dest->db)
        sqlite3_exec(dest->db, "ROLLBACK", NULL, NULL, NULL);
finished:
    if (backup)
        sqlite3_backup_finish(backup);
    sqlite3_finalize(operations);
    sqlite3_finalize(decisions);
    experiment_close(source);
    experiment_close(dest);
    if (!ok && created) {
        wchar_t full[32768];
        if (exp_store_local_path(new_path, full))
            DeleteFileW(full);
    }
    return ok;
}
