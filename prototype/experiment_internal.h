#ifndef EXPERIMENT_INTERNAL_H
#define EXPERIMENT_INTERNAL_H
/* prototype 내부 전용 계약으로 공개 API/ABI가 아니다. coordinator가 이 context,
 * core engine, Track과 transaction 경계를 소유한다. 책임별 파일은 나뉘었지만
 * 상태를 여러 opaque 객체로 분해한 것은 아니며 오류·metadata·transaction을 공유한다.
 * store는 실험 DB connection과 오류 버퍼를 빌리고 Track은 수정하지 않는다.
 * capture는 호출자 소유의 다음 Track을 준비하며 COMMIT 뒤에만 채택된다.
 * reducer/history/replay는 복사된 실험 근거를 사용하며 core 구조체를 보지 않는다.
 * 임시 statement는 선언에서 호출자에게 넘긴다고 명시한 경우 외에는 생성 함수가 finalize한다.
 * 실패 transaction의 rollback은 호출자가 맡는다. retention은 이 context와 독립적이다.
 */
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "experiment.h"
#include "sqlite3.h"
#define EXP_APP 1163415601
#define TICKS UINT64_C(10000000)
#define DAY (UINT64_C(86400) * TICKS)
#define ALL_METRICS 15u
typedef struct Track {
    uint32_t pid;
    uint64_t creation, utc;
    unsigned interrupted;
    int identity_valid;
} Track;
struct Experiment {
    sqlite3 *db;
    MonitorEngine *core;
    Track *tracks; /* 마지막으로 실험 DB에 commit한 회차의 연속 상태를 소유한다. */
    size_t track_count;
    uint64_t session;
    int readonly, replay, synthetic, fault, expired_prefix;
    char error[512];
    char *source, *pair;
    /* 결정 한 건의 replay 호출 동안만 빌린다. Experiment가 해제하지 않는다. */
    const ExperimentDecision *replayed_decision;
    const ExperimentEpisode *replayed_episode;
};

extern const double exp_fixed_limits[4];
char *exp_store_dup_text(const char *p); /* 호출자가 소유하는 복사본이며 호출자가 free한다. */
const char *exp_store_column_text(sqlite3_stmt *s, int col); /* 다음 step/reset/finalize까지 빌리는 문자열이다. */
void exp_store_db_error(Experiment *e, const char *what);
int exp_store_execute(Experiment *e, const char *sql);
int exp_store_prepare(Experiment *e, const char *sql, sqlite3_stmt **s); /* 호출자가 *s를 finalize한다. */
int exp_store_done(Experiment *e, sqlite3_stmt *s);
void exp_store_bind_real(sqlite3_stmt *s, int col, double value);
double exp_store_real_column(sqlite3_stmt *s, int col);
int exp_store_local_path(const char *path, wchar_t *full); /* full은 wchar_t 32768개를 담는 호출자 버퍼다. */
int exp_store_file_absent(const char *path);
char *exp_store_absolute_utf8(const char *path); /* 호출자가 소유하는 복사본이며 호출자가 free한다. */
void exp_store_copy_error(Experiment *e, char *error, size_t size);
int exp_store_open_sidecar(const char *path, int writable, Experiment **out, char *error, size_t size);
int exp_store_initialize_schema(Experiment *e);
int exp_store_record_operation(Experiment *e, int kind, uint64_t utc, uint64_t session, uint32_t pid,
                               uint64_t creation, unsigned mask, const char *reason, int64_t *id);
int exp_store_metric(Experiment *e, int64_t window, unsigned metric, const MonitorMetricReport *r, int continuity);
/* capture/reducer는 호출자가 연 실험 DB transaction 안에서 동작한다.
 * 보고서·이름·경로·replay 초기 요약 포인터는 호출 중에만 빌린다.
 * next_tracks는 capture만 수정하며 Track의 채택·해제는 coordinator만 수행한다.
 * committed_windows는 전체 source view를 빌려 값을 복사한다. coordinator는 복사 직후나
 * 실패 시 view를 해제한다. reducer는 core 상태에 접근하지 않는다. */
struct MonitorCommittedWindows;
int exp_capture_gap(Experiment *e, uint64_t utc, uint64_t session, uint32_t pid,
                    uint64_t creation, unsigned mask, const char *reason);
int exp_capture_window(Experiment *e, int64_t source_window, int64_t source_run, int64_t cutoff,
                       uint64_t session, uint32_t pid, uint64_t creation, const char *name, const char *path,
                       const MonitorMetricReport *reports, int feedback, uint64_t feedback_expires,
                       unsigned interrupted);
int exp_store_window(Experiment *e, int64_t source_window, int64_t source_run, int64_t cutoff,
                     uint64_t session, uint32_t pid, uint64_t creation, const char *name, const char *path,
                     const MonitorMetricReport *reports, int feedback, uint64_t feedback_expires, int64_t *stored_id);
int exp_capture_absent_gaps(Experiment *e, const ResourceBatch *batch);
int exp_capture_continuity(Experiment *e, const ResourceBatch *batch, Track *next_tracks);
int exp_capture_committed_windows(Experiment *e, int64_t cutoff, const struct MonitorCommittedWindows *windows,
                                  Track *next_tracks);
int exp_reduce_gap(Experiment *e, uint64_t utc, uint64_t session, uint32_t pid,
                   uint64_t creation, unsigned mask, const char *reason);
int exp_reduce_decision(Experiment *e, int64_t window_id, uint64_t session, uint32_t pid,
                        uint64_t creation, const char *name, const char *path, unsigned metric,
                        const MonitorMetricReport *report, ExperimentMethod method);
/* load는 호출자의 SQLite statement를 빌리고 name/path 복사본을 만든다.
 * 호출자는 성공 결과뿐 아니라 load 실패로 일부만 초기화된 행의 문자열도 해제한다. */
int exp_history_prepare_replay(Experiment *e, sqlite3_stmt **s); /* 호출자가 finalize한다. */
int exp_history_load_decision(sqlite3_stmt *s, ExperimentDecision *d);
#endif
