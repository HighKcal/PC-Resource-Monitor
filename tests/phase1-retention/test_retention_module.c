/* private retention object와 내장 SQLite만 링크하는 독립 경계 시험이다.
 * Experiment/core/collector/GUI나 제품 .c의 직접 포함 없이 동작한다.
 * Windows API를 호출하지 않아 user32/gdi32 import library를 별도로 요구하지 않는다. */
#include <stdio.h>
#include <string.h>
#include "experiment_retention.h"

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line=%u %s\n", (unsigned)__LINE__, #x); } } while (0)
#define NOW (UINT64_C(28) * UINT64_C(86400) * UINT64_C(10000000) + 10)

static void run(sqlite3 *db, const char *sql)
{
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    CHECK(rc == SQLITE_OK);
    if (rc != SQLITE_OK) printf("SQL_ERROR %s\n", sqlite3_errmsg(db));
}
static int64_t number(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    int64_t n = -1;
    CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    if (s && sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int64(s, 0);
    else CHECK(0);
    sqlite3_finalize(s);
    return n;
}
static sqlite3 *open_fixture(int ratings)
{
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    if (!db) return NULL;
    /* 보관 모듈에 필요한 열과 제약만 구성한다.
     * 전체 제품 schema와 공개 replay 검증은 test_retention.c가 맡는다. */
    run(db, "PRAGMA foreign_keys=ON; CREATE TABLE operation(id INTEGER PRIMARY KEY,kind INTEGER,utc INTEGER,session INTEGER,pid INTEGER,creation INTEGER,mask INTEGER); CREATE INDEX operation_time ON operation(utc); CREATE TABLE decision(id INTEGER PRIMARY KEY,window_id INTEGER NOT NULL REFERENCES operation(id) ON DELETE CASCADE,episode_id INTEGER); CREATE INDEX decision_window ON decision(window_id); CREATE TABLE episode(id INTEGER PRIMARY KEY,session INTEGER,pid INTEGER,creation INTEGER,metric INTEGER,last INTEGER,ended INTEGER); CREATE TABLE reduction(session INTEGER,pid INTEGER,creation INTEGER,metric INTEGER,method INTEGER,streak INTEGER,first INTEGER,episode INTEGER NOT NULL,PRIMARY KEY(session,pid,creation,metric,method)) WITHOUT ROWID; CREATE TABLE evaluation(id INTEGER PRIMARY KEY,created INTEGER,decision INTEGER,episode INTEGER); CREATE TABLE retention_state(expired_prefix INTEGER); INSERT INTO retention_state VALUES(0); INSERT INTO operation VALUES(1,0,1,1,3,9,0),(2,1,2,1,3,9,1); INSERT INTO decision VALUES(9,1,7); INSERT INTO episode VALUES(7,1,3,9,0,1,2); INSERT INTO reduction VALUES(1,3,9,0,1,3,1,7)");
    if (ratings)
        run(db, "WITH RECURSIVE n(x) AS(VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<1001) INSERT INTO evaluation SELECT x,100,9,7 FROM n");
    return db;
}
static sqlite3_stmt *borrowed_statement(sqlite3 *db)
{
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT 42", -1, &s, NULL) == SQLITE_OK);
    return s;
}
static void only_borrowed_statement(sqlite3 *db, sqlite3_stmt *borrowed)
{
    CHECK(sqlite3_next_stmt(db, NULL) == borrowed);
    CHECK(sqlite3_next_stmt(db, borrowed) == NULL);
    CHECK(!sqlite3_stmt_busy(borrowed));
}
static void close_fixture(sqlite3 *db, sqlite3_stmt *borrowed)
{
    CHECK(sqlite3_step(borrowed) == SQLITE_ROW);
    CHECK(sqlite3_column_int(borrowed, 0) == 42);
    CHECK(sqlite3_finalize(borrowed) == SQLITE_OK);
    CHECK(sqlite3_next_stmt(db, NULL) == NULL);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static void borrowed_connection_and_reference_closure(void)
{
    sqlite3 *db = open_fixture(1);
    sqlite3_stmt *borrowed = borrowed_statement(db);
    char error[128] = "keep caller error on success";
    run(db, "BEGIN IMMEDIATE");
    CHECK(experiment_retention_trim(db, 0, error, sizeof(error)));
    CHECK(!strcmp(error, "keep caller error on success"));
    CHECK(!sqlite3_get_autocommit(db));
    CHECK(number(db, "SELECT count(*) FROM operation") == 2);
    only_borrowed_statement(db, borrowed);
    run(db, "ROLLBACK; BEGIN IMMEDIATE");
    CHECK(experiment_retention_trim(db, NOW, error, sizeof(error)));
    CHECK(!sqlite3_get_autocommit(db));
    CHECK(!strcmp(error, "keep caller error on success"));
    CHECK(number(db, "SELECT count(*) FROM evaluation") == 1);
    CHECK(number(db, "SELECT count(*) FROM operation") == 2); /* 평가 대상 창과 종료 관측 단절을 함께 보호한다. */
    CHECK(number(db, "SELECT count(*) FROM episode") == 1);
    CHECK(number(db, "SELECT count(*) FROM reduction") == 0);
    CHECK(number(db, "SELECT expired_prefix FROM retention_state") == 1);
    only_borrowed_statement(db, borrowed);
    run(db, "ROLLBACK");
    CHECK(number(db, "SELECT count(*) FROM evaluation") == 1001);
    CHECK(number(db, "SELECT count(*) FROM reduction") == 1);
    CHECK(number(db, "SELECT expired_prefix FROM retention_state") == 0);
    run(db, "BEGIN IMMEDIATE");
    CHECK(experiment_retention_trim(db, NOW, NULL, 0));
    CHECK(!sqlite3_get_autocommit(db));
    run(db, "COMMIT");
    CHECK(number(db, "SELECT count(*) FROM evaluation") == 1);
    run(db, "BEGIN IMMEDIATE");
    CHECK(experiment_retention_trim(db, NOW, error, sizeof(error)));
    CHECK(!sqlite3_get_autocommit(db));
    run(db, "COMMIT");
    CHECK(number(db, "SELECT count(*) FROM evaluation") == 0);
    CHECK(number(db, "SELECT count(*) FROM operation") == 0);
    CHECK(number(db, "SELECT count(*) FROM episode") == 0);
    only_borrowed_statement(db, borrowed);
    close_fixture(db, borrowed);
    printf("MODULE_OWNERSHIP borrowed_connection_statement=preserved caller_transaction=preserved bounded_reference_cleanup=ok\n");
}
static int deny_episode_delete(void *context, int action, const char *one, const char *two, const char *three, const char *four)
{
    unsigned *hits = context;
    (void)two; (void)three; (void)four;
    if (action == SQLITE_DELETE && one && !strcmp(one, "episode")) { ++*hits; return SQLITE_DENY; }
    return SQLITE_OK;
}
static void caller_owns_failure_rollback(int step_failure)
{
    sqlite3 *db = open_fixture(0);
    sqlite3_stmt *borrowed = borrowed_statement(db);
    char error[128] = "unchanged";
    unsigned hits = 0;
    if (step_failure)
        run(db, "CREATE TEMP TRIGGER fail_retention BEFORE DELETE ON episode BEGIN SELECT RAISE(ABORT,'module injected step failure'); END");
    else CHECK(sqlite3_set_authorizer(db, deny_episode_delete, &hits) == SQLITE_OK);
    run(db, "BEGIN IMMEDIATE");
    CHECK(!experiment_retention_trim(db, NOW, error, sizeof(error)));
    CHECK(strstr(error, step_failure ? "experiment step:" : "experiment prepare:") == error);
    CHECK(!sqlite3_get_autocommit(db));
    CHECK(number(db, "SELECT count(*) FROM operation") == 0); /* 앞선 삭제는 아직 호출자 transaction 안의 미확정 변경이다. */
    CHECK(number(db, "SELECT count(*) FROM episode") == 1);
    only_borrowed_statement(db, borrowed);
    if (!step_failure) { CHECK(hits == 1); CHECK(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK); }
    run(db, "ROLLBACK");
    CHECK(number(db, "SELECT count(*) FROM operation") == 2);
    CHECK(number(db, "SELECT count(*) FROM decision") == 1);
    CHECK(number(db, "SELECT count(*) FROM reduction") == 1);
    CHECK(number(db, "SELECT expired_prefix FROM retention_state") == 0);
    if (step_failure) run(db, "DROP TRIGGER fail_retention");
    run(db, "BEGIN IMMEDIATE");
    CHECK(experiment_retention_trim(db, NOW, error, sizeof(error)));
    run(db, "COMMIT");
    CHECK(number(db, "SELECT count(*) FROM episode") == 0);
    only_borrowed_statement(db, borrowed);
    close_fixture(db, borrowed);
    printf("MODULE_FAILURE stage=%s error_written=1 statements_finalized=1 caller_rollback=ok\n", step_failure ? "step" : "prepare");
}
int main(void)
{
    borrowed_connection_and_reference_closure();
    caller_owns_failure_rollback(0); caller_owns_failure_rollback(1);
    printf("RETENTION_MODULE_RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
