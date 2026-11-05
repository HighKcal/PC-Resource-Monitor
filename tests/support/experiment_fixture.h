#ifndef EXPERIMENT_FIXTURE_H
#define EXPERIMENT_FIXTURE_H
/* 합성 시험·벤치마크용 접근 경계이며 제품 공개 API가 아니다.
 * handle과 statement 문자열은 빌린다. 호출자는 직접 준비한 SQLite statement,
 * 시작한 transaction, 공개 API에서 반환받은 snapshot을 소유한다.
 * 초기 근거 구성은 합성 실험 DB에서 실제 capture/reducer를 실행한다.
 * 검증 조건과 독립 비교기 기대값 계산은 호출하는 시험에 둔다.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <wchar.h>
#include "experiment.h"
#include "sqlite3.h"
#ifndef TICKS
#define TICKS UINT64_C(10000000)
#endif
#ifndef DAY
#define DAY (UINT64_C(86400) * TICKS)
#endif
#ifndef ALL_METRICS
#define ALL_METRICS 15u
#endif
#define EXPERIMENT_FIXTURE_APP_ID 1163415601
sqlite3 *experiment_fixture_db(Experiment *e);
MonitorEngine *experiment_fixture_core(Experiment *e);
sqlite3 *experiment_fixture_core_db(Experiment *e);
uint64_t experiment_fixture_session(Experiment *e);
int experiment_fixture_fault(Experiment *e);
int experiment_fixture_synthetic(Experiment *e);
int experiment_fixture_set_source(Experiment *e, const char *source);
int experiment_fixture_file_absent(const char *path);
int experiment_fixture_local_path(const char *path, wchar_t *full);
int experiment_fixture_execute(Experiment *e, const char *sql);
int experiment_fixture_prepare(Experiment *e, const char *sql, sqlite3_stmt **s);
const char *experiment_fixture_column_text(sqlite3_stmt *s, int col);
double experiment_fixture_fixed_limit(unsigned metric);
int experiment_fixture_trim(Experiment *e, uint64_t utc);
int experiment_fixture_gap(Experiment *e, uint64_t utc, uint64_t session, uint32_t pid,
                           uint64_t creation, unsigned mask, const char *reason);
int experiment_fixture_store_window(Experiment *e, int64_t source_window, int64_t source_run, int64_t cutoff,
                                    uint64_t session, uint32_t pid, uint64_t creation, const char *name,
                                    const char *path, const MonitorMetricReport *reports, int feedback,
                                    uint64_t feedback_expires, unsigned interrupted);
#endif
