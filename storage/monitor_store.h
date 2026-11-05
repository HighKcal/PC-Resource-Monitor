#ifndef MONITOR_STORE_H
#define MONITOR_STORE_H
#include "monitor_types.h"
/* worker 전용 DB connection·statement 소유자. 입력은 호출 중에만 빌린다.
 * 오류 문자열은 store가 소유하며 다음 오류나 close 전까지 유효하다. engine이
 * begin → run/raw/window/feedback 조회/touch/trim → commit을 조율하며, 이 범위의
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
 * 복사한다. close는 statement 정리와 session 종료 기록을 시도한 뒤 store를 해제한다. */
int monitor_store_open(const char *path, int synthetic, MonitorStore **out, char *error, size_t size);
void monitor_store_close(MonitorStore *store);
const char *monitor_store_error(const MonitorStore *store);
int64_t monitor_store_session(const MonitorStore *store);
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
int monitor_store_trim(MonitorStore *store, uint64_t now);
int monitor_store_commit(MonitorStore *store);
void monitor_store_rollback(MonitorStore *store); /* 앞선 오류 문자열을 덮어쓰지 않는다. */
/* 수집 실패 기록은 단일 autocommit INSERT이며 피드백 쓰기는 별도 트랜잭션을 연다. */
int monitor_store_record_failure(MonitorStore *store, uint64_t utc, unsigned status);
int monitor_store_feedback(MonitorStore *store, int64_t program_id, MonitorFeedback kind, uint64_t now);
int monitor_store_stats(MonitorStore *store, MonitorStorageStats *out);
#endif
