#ifndef PROCESS_GROUPING_H
#define PROCESS_GROUPING_H
#include "monitor.h"

/* GUI용 읽기 전용 관계 추정이며 engine/collector ABI를 바꾸지 않는다.
 * OS 부모 대조 성공도 추정이다. 앱 소속이나 실제 생성 호출자를 확정하지 않는다.
 * pg_collect_windows가 연 핸들은 반환 전에 닫는다. 반환 snapshot은 호출자가 소유하고
 * pg_snapshot_free로 해제한다. tracker는 별도로 생성·초기화·해제한다. */
#define PG_NONE ((size_t)-1)
#define PG_MAX_DEPTH 64u
#define PG_MAX_CACHE 8192u
/* creation은 UTC 100ns 생성시각이며 PID와 묶어 인스턴스를 구분한다. 단조시각은 아니다. */
typedef struct PgIdentity { uint32_t pid; uint64_t creation; } PgIdentity;
typedef enum PgGrade {
    PG_PID_ONLY, PG_CURRENT_INFERRED, PG_HISTORICAL_INFERRED,
    PG_IDENTITY_UNAVAILABLE, PG_PROBE_FAILED, PG_DISABLED
} PgGrade;
typedef enum PgCut {
    PG_CUT_NONE, PG_CUT_UNVERIFIED, PG_CUT_PARENT_ABSENT, PG_CUT_IDENTITY,
    PG_CUT_DUPLICATE_PID, PG_CUT_SELF, PG_CUT_TIME_ORDER, PG_CUT_CYCLE,
    PG_CUT_HOST, PG_CUT_DEPTH, PG_CUT_GROUP_LIMIT, PG_CUT_PARENT_CHANGED
} PgCut;
/* probe 배열의 인덱스는 원본 MonitorSnapshot 행과 같다. PG_CURRENT_INFERRED는 양쪽
 * 핸들의 생성시각이 행 신원과 일치하고, 핸들 유지 중 두 번째 Toolhelp 부모 PID도 같으며,
 * 이후 양쪽 Wait(0)이 WAIT_TIMEOUT이었다는 뜻이다. 순수 resolver가 tuple·생성 순서·
 * 순환·표시 경계를 다시 검사하므로 이 등급만으로 표시 연결을 보장하지 않는다. */
typedef struct PgProbe {
    uint32_t parent_pid, error;
    uint64_t parent_creation;
    int parent_pid_valid;
    PgGrade grade;
} PgProbe;
typedef struct PgRelation {
    uint32_t raw_parent_pid, error;
    int raw_parent_valid;
    PgIdentity parent;
    PgGrade grade;
    PgCut cut;
    uint64_t last_matched_utc;
    size_t display_parent, children, descendants;
    unsigned depth;
} PgRelation;
typedef struct PgIndex { uint32_t pid; uint64_t creation; size_t row; int valid; } PgIndex;
typedef struct PgSnapshot {
    PgRelation *relations;
    size_t *order; /* 전체를 펼친 순서로 원본 행이 정확히 한 번씩 들어간다. */
    PgIndex *index;
    size_t count, inferred, historical, roots;
    uint32_t probe_error;
} PgSnapshot;
typedef struct PgTracker PgTracker;
typedef struct PgUiState { PgIdentity *collapsed; size_t count; } PgUiState;

PgTracker *pg_tracker_create(void);
void pg_tracker_reset(PgTracker *);
void pg_tracker_destroy(PgTracker *);
/* 할당 실패는 0과 *out=NULL을 반환한다. 호출자는 모든 원본 행을 개별 목록으로 표시한다.
 * 캐시는 현재 남아 있고 중복 없는 child tuple만 PG_MAX_CACHE개까지 보존한다. */
int pg_resolve(PgTracker *, const MonitorSnapshot *, const PgProbe *, PgSnapshot **out);
/* stop_event는 Windows 수동 재설정 event HANDLE 또는 NULL이며 호출자가 소유한다.
 * 취소·Toolhelp 실패도 할당 가능하면 개별 목록 표시용 메타데이터 snapshot으로 반환한다. */
int pg_collect_windows(PgTracker *, const MonitorSnapshot *, void *stop_event, PgSnapshot **out);
void pg_snapshot_free(PgSnapshot *);
PgIdentity pg_row_identity(const MonitorRow *);
int pg_identity_equal(PgIdentity, PgIdentity);
size_t pg_find_identity(const PgSnapshot *, PgIdentity);
const char *pg_grade_name(PgGrade);
const char *pg_cut_name(PgCut);
void pg_ui_reset(PgUiState *);
int pg_ui_reconcile(PgUiState *, const PgSnapshot *);
int pg_ui_collapsed(const PgUiState *, PgIdentity);
int pg_ui_set_collapsed(PgUiState *, PgIdentity, int collapsed);
/* out에 원본 행 인덱스를 쓴다. 호출자는 원본 행 수만큼 공간을 준비한다.
 * flat=1 또는 관계 snapshot=NULL이면 원본 순서로 모든 행을 보존한다. */
size_t pg_visible_rows(const PgSnapshot *, const MonitorSnapshot *, const PgUiState *, int flat, size_t *out);
#endif
