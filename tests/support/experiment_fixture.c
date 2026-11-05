/* 현재 제품은 책임별 object를 따로 빌드하며 시험은 fixture 헤더로 접근한다.
 * 이 호환 어댑터의 비분리 빌드 분기만 과거 단일 구현 파일을 직접 포함한다. */
#ifdef EXPERIMENT_MODULE_SPLIT
#include "experiment_internal.h"
#include "experiment_retention.h"
#include "monitor_fixture.h"
#include <stdlib.h>
#define FIXTURE_EXECUTE exp_store_execute
#define FIXTURE_PREPARE exp_store_prepare
#define FIXTURE_WINDOW exp_capture_window
#define FIXTURE_GAP exp_capture_gap
#define FIXTURE_DUP exp_store_dup_text
#define FIXTURE_ABSENT exp_store_file_absent
#define FIXTURE_LOCAL_PATH exp_store_local_path
#else
#include "experiment.c"
#define FIXTURE_EXECUTE execute
#define FIXTURE_PREPARE prepare
#define FIXTURE_WINDOW store_window
#define FIXTURE_GAP gap
#define FIXTURE_DUP dup_text
#define FIXTURE_ABSENT file_absent
#define FIXTURE_LOCAL_PATH local_path
#endif
#include "experiment_fixture.h"
#include "monitor_fixture.h"

sqlite3 *experiment_fixture_db(Experiment *e) { return e->db; }
MonitorEngine *experiment_fixture_core(Experiment *e) { return e->core; }
sqlite3 *experiment_fixture_core_db(Experiment *e) { return monitor_fixture_db(e->core); }
uint64_t experiment_fixture_session(Experiment *e) { return e->session; }
int experiment_fixture_fault(Experiment *e) { return e->fault; }
int experiment_fixture_synthetic(Experiment *e) { return e->synthetic; }
int experiment_fixture_file_absent(const char *path) { return FIXTURE_ABSENT(path); }
int experiment_fixture_local_path(const char *path, wchar_t *full) { return FIXTURE_LOCAL_PATH(path, full); }
int experiment_fixture_set_source(Experiment *e, const char *source)
{
    /* 기존 합성 초기화의 해제 후 복사 순서를 유지한다. */
    free(e->source);
    e->source = FIXTURE_DUP(source);
    return e->source != NULL;
}
int experiment_fixture_execute(Experiment *e, const char *sql) { return FIXTURE_EXECUTE(e, sql); }
int experiment_fixture_prepare(Experiment *e, const char *sql, sqlite3_stmt **s)
{ return FIXTURE_PREPARE(e, sql, s); }
const char *experiment_fixture_column_text(sqlite3_stmt *s, int col)
{
    const unsigned char *p = sqlite3_column_text(s, col);
    return p ? (const char *)p : "";
}
/* 제품 정책 객체에서 기대값을 끌어오지 않도록 기존 합성 입력 상수를 별도로 둔다. */
double experiment_fixture_fixed_limit(unsigned metric)
{
    static const double limits[4] = {25, 536870912, 10485760, 10485760};
    return limits[metric];
}
int experiment_fixture_trim(Experiment *e, uint64_t utc)
{
#ifdef EXPERIMENT_RETENTION_INTERNAL_H
    return experiment_retention_trim(e->db, utc, e->error, sizeof(e->error));
#else
    return trim(e, utc);
#endif
}
int experiment_fixture_gap(Experiment *e, uint64_t utc, uint64_t session, uint32_t pid,
                           uint64_t creation, unsigned mask, const char *reason)
{ return FIXTURE_GAP(e, utc, session, pid, creation, mask, reason); }
int experiment_fixture_store_window(Experiment *e, int64_t source_window, int64_t source_run, int64_t cutoff,
                                    uint64_t session, uint32_t pid, uint64_t creation, const char *name,
                                    const char *path, const MonitorMetricReport *reports, int feedback,
                                    uint64_t feedback_expires, unsigned interrupted)
{
    return FIXTURE_WINDOW(e, source_window, source_run, cutoff, session, pid, creation, name, path,
                          reports, feedback, feedback_expires, interrupted);
}
