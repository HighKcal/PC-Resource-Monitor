#ifndef MONITOR_SOURCE_H
#define MONITOR_SOURCE_H
#include "monitor.h"
/* 내부 동기 worker 경계이며 공개 API/ABI가 아니다. 참조는 하나의 살아 있는 engine과
 * 원래 반환된, 해제·수정하지 않은 snapshot에 속한다. 이후 engine 쓰기 시도나 reset은
 * 참조를 무효화한다. 해제된 snapshot·닫힌 engine은 계약 밖이며 참조를 직렬화하거나
 * 다른 engine으로 이전하지 않는다. */
typedef struct MonitorCommitRef {
    uint64_t engine_cookie, mutation_epoch, sequence;
    int64_t session_id, run_id, history_cutoff;
} MonitorCommitRef;
enum { MONITOR_SOURCE_DUPLICATE_TUPLE = 1u, MONITOR_SOURCE_METADATA_MISMATCH = 2u };
typedef struct MonitorCommittedWindow {
    int64_t window_id, run_id;
    size_t row_index;
    const MonitorRow *row;
    unsigned match_flags;
} MonitorCommittedWindow;
typedef struct MonitorCommittedWindows { MonitorCommittedWindow *items; size_t count; } MonitorCommittedWindows;
/* 기존 ingest는 ref=NULL로 위임하여 source 조회·배열·할당을 생략한다.
 * 실패 시 참조는 0이며, 완료창이 없는 성공 회차도 참조를 받는다. mutation epoch가
 * 소진되면 core 저장은 계속되지만 이 engine의 참조 발급은 영구 중단된다.
 * 참조는 0으로 남고 source 조회도 거부된다. */
int monitor_ingest_with_commit_ref(MonitorEngine *engine, const ResourceBatch *batch,
    const char *const *synthetic_paths, MonitorSnapshot **out, MonitorCommitRef *ref);
/* COMMIT 이후에만 조회한다. out은 비어 있어야 하며 성공한 전체 배열은 호출자가
 * 소유한다. row·이름·경로·report는 snapshot을 빌리므로 snapshot과 참조의 수명 안에서만
 * 사용하고 windows_free로 배열을 해제한다. 실패 시 부분 결과를 공개하지 않는다.
 * SQL 오류는 SQLite errmsg, 검증 오류는 연결 실패 이유를 제공한다. 중복 신원은 기존의
 * 첫 일치 행을 유지하며 모호성·메타데이터 불일치를 플래그로 알린다. */
int monitor_source_read_committed(MonitorEngine *engine, const MonitorCommitRef *ref,
    const MonitorSnapshot *snapshot, MonitorCommittedWindows *out);
void monitor_source_windows_free(MonitorCommittedWindows *windows);
/* 짝 생성은 source와 sidecar의 별도 COMMIT을 유지한다. 실패 오류는 호출자가
 * 기존 Source pairing 접두어와 함께 표시할 SQLite errmsg다. */
int monitor_source_create_pair(MonitorEngine *engine, const char *pair);
int monitor_source_pair_matches(const char *source_path, const char *pair);
int64_t monitor_source_session(const MonitorEngine *engine);
typedef struct MonitorSourceDiagnostics {
    uint64_t utc, rows, raw_rows, identity_unavailable, path_unavailable;
    uint64_t metric_missing[MONITOR_METRICS];
    int source_available, available, status;
} MonitorSourceDiagnostics;
/* 짝이 확인된 source의 읽기 전용 진단. 후속 조회가 실패해도 이미 읽은 UTC·상태·행 수는
 * 유지하며, 전체 자료 조회 성공 여부는 available로 구분한다. */
void monitor_source_diagnostics(const char *source_path, const char *pair, MonitorSourceDiagnostics *out);
#endif
