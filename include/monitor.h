#ifndef MONITOR_H
#define MONITOR_H
#include "monitor_types.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MonitorEngine MonitorEngine;

/* worker 전용 API. 새 DB에 식별 정보를 기록하며, 알 수 없는·미래 스키마나
 * real/synthetic 불일치는 이관 없이 거부한다. 경로는 로컬 디스크의 UTF-8 경로이며
 * 오류 버퍼는 호출자가 소유한다. */
int monitor_open(const char *path_utf8, int synthetic, MonitorEngine **out,
    char *error, size_t error_size);
/* 한 배치의 원시 자료와 집계를 하나의 트랜잭션으로 저장한다. 실제 DB에서는
 * synthetic_paths가 NULL이어야 한다. 합성 경로는 검증된 경로의 시험 대체값이며 실제
 * 신원이 아니다. 실패 시 *out은 NULL, 트랜잭션은 rollback되고 관측창·연속 초과 상태는
 * 초기화된다. snapshot과 모든 문자열은 배치·engine과 독립적이며 호출자가 소유한다. */
int monitor_ingest(MonitorEngine *, const ResourceBatch *,
    const char *const *synthetic_paths, MonitorSnapshot **out);
int monitor_record_failure(MonitorEngine *, uint64_t utc, unsigned status);
void monitor_reset(MonitorEngine *); /* 미완료 관측창과 연속 초과 상태를 버린다. */
int monitor_feedback(MonitorEngine *, int64_t program_id, MonitorFeedback,
    uint64_t now_utc_100ns);
const char *monitor_error(const MonitorEngine *);
const char *monitor_storage_version(void);
/* 선택적 읽기 전용 worker 진단. 행 수·페이지를 조회하므로 시간이 걸릴 수 있다. */
int monitor_storage_stats(MonitorEngine *, MonitorStorageStats *);
void monitor_snapshot_free(MonitorSnapshot *);
void monitor_close(MonitorEngine *);
/* 같은 핸들에서 생성시각을 검증한 뒤에만 malloc으로 소유한 UTF-8 경로를 반환한다.
 * 호출자가 free한다. 권한을 높이지 않으며 생성시각 불일치 오류는 ERROR_INVALID_DATA다. */
char *monitor_query_path(uint32_t pid, uint64_t expected_creation, uint32_t *error);
uint64_t monitor_utc_now(void);
const char *monitor_state_name(MonitorState);
const char *monitor_metric_name(unsigned);
#ifdef __cplusplus
}
#endif
#endif
