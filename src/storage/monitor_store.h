#ifndef MONITOR_STORE_H
#define MONITOR_STORE_H
#include "monitor_types.h"
/* worker 전용 DB connection·statement 소유자. 입력은 호출 중에만 빌린다.
 * 오류 문자열은 store가 소유하며 다음 오류나 close 전까지 유효하다. engine이
 * begin → run/raw/window/feedback 조회/touch → commit을 조율하며, 이 범위의
 * 실패는 호출자가 rollback한다. 프로세스 인스턴스 상태는 보관하지 않는다. */
typedef struct MonitorStore MonitorStore;
typedef struct MonitorWindowHeader {
    int64_t run_id, program_id;
    uint32_t pid;
    uint64_t creation, start, end;
    double duration;
} MonitorWindowHeader;
/* 호출자 스택에 두며 임시 metric statement 하나를 소유한다. begin 성공 뒤에는
 * Baseline·metric 저장이 실패해도 반드시 end로 해제한다. */
typedef struct MonitorWindowWrite { void *statement; int64_t window_id; } MonitorWindowWrite;
/* open 성공 시 store 소유권을 넘기고 실패 시 NULL을 남긴다. 오류는 호출자 버퍼에
 * 복사한다. 개방 helper의 임시 statement는 각각 해제하고, store는 연결과 재사용 statement를
 * 소유한다. close는 미완료 쓰기의 rollback 성공을 확인한 뒤 성공적으로 열린 session의
 * 종료 기록을 시도한다. rollback/종료 기록 실패 또는 개방 미완료는 ended=NULL로 남긴다. */
int monitor_store_open(const char *path, int synthetic, MonitorStore **out, char *error, size_t size);
void monitor_store_close(MonitorStore *store);
const char *monitor_store_error(const MonitorStore *store);
int64_t monitor_store_session(const MonitorStore *store);
/* 회차 트랜잭션 시작과 과거 창 ID 상한 확보를 함께 한다. 성공 뒤 commit/rollback은
 * 호출자 책임이다. 실패하면 시작한 작업만 rollback하며 다른 caller 트랜잭션은 거부한다. */
int monitor_store_begin(MonitorStore *store, int64_t *history_cutoff);
int monitor_store_insert_run(MonitorStore *store, const ResourceBatch *batch, int64_t *run_id);
int64_t monitor_store_program_id(MonitorStore *store, const char *path, const char *name, uint64_t utc);
int monitor_store_write_raw(MonitorStore *store, int64_t run_id, const ResourceData *raw,
    int64_t program_id, uint32_t path_error);
int monitor_store_begin_window(MonitorStore *store, const MonitorWindowHeader *header, MonitorWindowWrite *scope);
int monitor_store_write_metric(MonitorStore *store, MonitorWindowWrite *scope, unsigned metric, const MonitorMetricReport *report);
void monitor_store_end_window(MonitorWindowWrite *scope);
/* 피드백 출력은 복사된 값이다. 일치하는 행이 없으면 기존 출력값을 유지한다. */
int monitor_store_read_feedback(MonitorStore *store, int64_t program_id, uint64_t now,
    MonitorFeedback *feedback, uint64_t *expires);
int monitor_store_touch_run_programs(MonitorStore *store, int64_t run_id, uint64_t now);
/* 독립 정리 트랜잭션 하나를 소유한다. 실패는 해당 정리만 rollback한다. 앞서 커밋한 회차나
 * 피드백의 성공을 바꾸지 않으며, engine이 다음 성공한 쓰기 뒤 재호출한다. */
int monitor_store_trim(MonitorStore *store, uint64_t now);
/* aggregate_changed는 이 정리에서 완료창 삭제가 COMMIT된 경우에만 1이다. 실패/삭제 없음은
 * 0이며 추가 조회 SQL을 실행하지 않는다. engine은 최근 비교의 값·anchor 양쪽을 재선택한다. */
int monitor_store_trim_with_result(MonitorStore *store, uint64_t now, int *aggregate_changed);
int monitor_store_commit(MonitorStore *store);
void monitor_store_rollback(MonitorStore *store); /* 앞선 오류 문자열을 덮어쓰지 않는다. */
/* 앞선 rollback 실패만 재시도한다. 호출자가 연 다른 tx는 변경하지 않고 거부한다. */
int monitor_store_require_idle(MonitorStore *store);
/* 실패 rollback만 복구한 뒤 읽는다. 정상 caller tx와 원래 SQL 오류는 보존한다. */
int monitor_store_recover_pending(MonitorStore *store);
/* 수집 실패 기록은 단일 autocommit INSERT이며 보관 정리를 호출하지 않는다. */
int monitor_store_record_failure(MonitorStore *store, uint64_t utc, unsigned status);
/* engine이 검증한 요청과 만료 시각을 받는다. 기존 억제 취소와 새 피드백 삽입을 함께 commit하며,
 * 실패 시 둘 다 rollback을 시도한다. rollback도 실패하면 pending으로 남겨 다음 경계에서 복구한다.
 * 이미 열린 다른 호출자 트랜잭션을 끝내지 않는다. 보관 정리는 성공 뒤 engine이 맡는다. */
int monitor_store_commit_feedback(MonitorStore *store, int64_t program_id, MonitorFeedback kind,
    uint64_t now, uint64_t expires);
int monitor_store_stats(MonitorStore *store, MonitorStorageStats *out);
#endif
