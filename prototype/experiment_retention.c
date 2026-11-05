/* 호출자가 연 transaction 안에서 28일 보관 정리와 잔존 참조 보호를 수행한다.
 * DB·UTC·오류 버퍼만 받아 공유 Experiment context 없이 독립적으로 사용한다. */
#include <stdio.h>
#include "experiment_retention.h"

#define DAY_TICKS (UINT64_C(86400) * UINT64_C(10000000))

static void retention_error(sqlite3 *db, char *error, size_t size, const char *what)
{
    if (error && size)
        snprintf(error, size, "%s: %s (%d)", what, sqlite3_errmsg(db), sqlite3_extended_errcode(db));
}
static int retention_prepare(sqlite3 *db, const char *sql, sqlite3_stmt **s, char *error, size_t size)
{
    if (sqlite3_prepare_v2(db, sql, -1, s, NULL) == SQLITE_OK)
        return 1;
    retention_error(db, error, size, "experiment prepare");
    return 0;
}
static int retention_done(sqlite3 *db, sqlite3_stmt *s, char *error, size_t size)
{
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE)
        retention_error(db, error, size, "experiment step");
    return rc == SQLITE_DONE;
}
int experiment_retention_trim(sqlite3 *db, uint64_t utc, char *error, size_t error_size)
{
    sqlite3_stmt *s = NULL;
    int ok;
    uint64_t cutoff = utc > 28 * DAY_TICKS ? utc - 28 * DAY_TICKS : 0;
    /* 종류별 삭제 대상은 최대 1000개다. 연쇄 삭제 행 수나 실행 시간의 상한은 아니다.
       미처리분이 남을 수 있으므로 이번 transaction 뒤에도 남는 참조의 대상을 보호한다.
       사건 요약의 활성화·마지막 결정 ID에 해당하는 근거는 이미 만료됐을 수 있다. */
    if (!retention_prepare(
            db,
            "DELETE FROM evaluation WHERE id IN(SELECT id FROM evaluation WHERE created<? OR decision IN(SELECT d.id FROM decision d JOIN operation o ON o.id=d.window_id WHERE o.utc<?) OR episode IN(SELECT id FROM episode WHERE max(last,ended)<?) OR (decision IS NOT NULL AND decision NOT IN(SELECT id FROM decision)) OR (episode IS NOT NULL AND episode NOT IN(SELECT id FROM episode)) LIMIT 1000)",
            &s, error, error_size))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    sqlite3_bind_int64(s, 2, (int64_t)cutoff);
    sqlite3_bind_int64(s, 3, (int64_t)cutoff);
    ok = retention_done(db, s, error, error_size);
    sqlite3_finalize(s);
    if (!ok)
        return 0;
    if (!retention_prepare(db, "UPDATE retention_state SET expired_prefix=1 WHERE EXISTS(SELECT 1 FROM operation WHERE utc<?)",
                 &s, error, error_size))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    ok = retention_done(db, s, error, error_size);
    sqlite3_finalize(s);
    if (!ok)
        return 0;
    if (!retention_prepare(
            db,
            "WITH rated_episode(id) AS(SELECT episode FROM evaluation WHERE episode IS NOT NULL UNION SELECT d.episode_id FROM evaluation v JOIN decision d ON d.id=v.decision WHERE d.episode_id IS NOT NULL) "
            "DELETE FROM operation WHERE id IN(SELECT id FROM operation WHERE utc<? AND id NOT IN(SELECT window_id FROM decision WHERE id IN(SELECT decision FROM evaluation WHERE decision IS NOT NULL) OR episode_id IN(SELECT id FROM rated_episode) UNION SELECT o.id FROM rated_episode r JOIN episode p ON p.id=r.id JOIN operation o ON o.utc=p.ended WHERE o.kind=1 AND (o.session=0 OR o.session=p.session) AND (o.pid=0 OR (o.pid=p.pid AND o.creation=p.creation)) AND (o.mask&(1<<p.metric))<>0) ORDER BY id LIMIT 1000)",
            &s, error, error_size))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    ok = retention_done(db, s, error, error_size);
    sqlite3_finalize(s);
    if (!ok)
        return 0;
    /* 연속 상태나 평가가 1000개를 넘어 사건 삭제가 미뤄져도 만료된 연속 상태는 정리한다. */
    if (!retention_prepare(
            db,
            "DELETE FROM reduction WHERE (session,pid,creation,metric,method) IN(SELECT session,pid,creation,metric,method FROM reduction WHERE (episode<>0 AND (episode NOT IN(SELECT id FROM episode) OR episode IN(SELECT id FROM episode WHERE max(last,ended)<?))) OR (episode=0 AND first<?) LIMIT 1000)",
            &s, error, error_size))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    sqlite3_bind_int64(s, 2, (int64_t)cutoff);
    ok = retention_done(db, s, error, error_size);
    sqlite3_finalize(s);
    if (!ok)
        return 0;
    if (!retention_prepare(db, "DELETE FROM episode WHERE id IN(SELECT id FROM episode WHERE max(last,ended)<? AND id NOT IN(SELECT episode_id FROM decision WHERE episode_id IS NOT NULL) AND id NOT IN(SELECT episode FROM reduction WHERE episode<>0) AND id NOT IN(SELECT episode FROM evaluation WHERE episode IS NOT NULL) LIMIT 1000)",
                 &s, error, error_size))
        return 0;
    sqlite3_bind_int64(s, 1, (int64_t)cutoff);
    ok = retention_done(db, s, error, error_size);
    sqlite3_finalize(s);
    return ok;
}
