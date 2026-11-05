#ifndef PROGRAM_GROUPS_H
#define PROGRAM_GROUPS_H
#include "process_grouping.h"

typedef enum PgOwnerKind { PG_OWNER_AUTO, PG_OWNER_USER, PG_OWNER_SHARED } PgOwnerKind;
typedef struct PgGroupKey { PgOwnerKind kind; PgIdentity anchor; uint64_t id; } PgGroupKey;
/* CPU value는 %, I/O value는 B/s, RAM bytes는 Working Set bytes 합이다. RAM value는
 * 기존 MiB 환산값이며 GUI는 bytes에서 decimal GB로 변환한다. interval 범위 단위는 초다.
 * valid/total은 유효/전체 구성원 수이며 overflow 값은 유효 합계로 표시하지 않는다. */
typedef struct PgSum {
    double value, interval_min, interval_max;
    uint64_t bytes;
    size_t valid, total;
    int overflow;
} PgSum;
typedef struct PgGroup {
    PgGroupKey key;
    char label[256];
    size_t first, count;
    PgSum metric[MONITOR_METRICS];
    uint64_t observed_min, observed_max;
    size_t scope_splits, historical;
} PgGroup;
typedef struct PgGroups {
    PgGroup *groups;
    size_t count, row_count;
    size_t *members, *owner, *order; /* members는 구성원 행, owner는 행별 그룹, order는 다중 구성원·신원 있음·없음 순서다. */
} PgGroups;
/* 소속·복사한 그룹 이름·이동 대상·펼침은 UI 세션 소유다. 재개/재시작 시 초기화하며
 * DB에 저장하지 않는다. pg_target_label 반환 문자열도 이 상태의 수명을 빌린다. */
typedef struct PgMembership PgMembership;
PgMembership *pg_membership_create(void);
void pg_membership_reset(PgMembership *);
void pg_membership_destroy(PgMembership *);
/* UI 스레드 전용이다. 부재·모호한 tuple의 지정을 정리하고 불변 자원·관계 위에 최신
 * 세션 지정을 적용한다. 모든 원본 행은 합계 한 곳에만 속한다. 입력은 빌리며 출력은
 * 호출자가 pg_groups_free로 해제한다. OOM이면 *out=NULL이므로 원본 개별 목록을 표시한다. */
int pg_groups_build(PgMembership *,const MonitorSnapshot *,const PgSnapshot *,PgGroups **out);
void pg_groups_free(PgGroups *);
int pg_group_key_equal(PgGroupKey,PgGroupKey);
size_t pg_group_find(const PgGroups *,PgGroupKey);
/* 현재 구성원만 한 번에 지정하며 미래 자식에게 상속하는 규칙은 만들지 않는다.
 * set_target이면 자동 그룹도 세션 지정으로 확정한 뒤 이동 대상 ID를 기억한다. */
int pg_confirm_members(PgMembership *,const PgGroups *,size_t group,const MonitorSnapshot *,const PgSnapshot *,int set_target);
/* 개별 대상을 지정하면 그 tuple 하나로 시작하거나 기존 USER 그룹을 선택한다.
 * 같은 자동 실행 계통의 나머지 구성원까지 확정하지 않는다. */
int pg_set_process_target(PgMembership *,const MonitorSnapshot *,const PgSnapshot *,size_t row);
int pg_move_to_target(PgMembership *,const MonitorSnapshot *,const PgSnapshot *,size_t row);
int pg_separate_shared(PgMembership *,const MonitorSnapshot *,const PgSnapshot *,size_t row);
int pg_restore_auto(PgMembership *,PgIdentity);
uint64_t pg_target_id(const PgMembership *);
const char *pg_target_label(const PgMembership *);
int pg_group_expanded(const PgMembership *,PgGroupKey);
int pg_group_set_expanded(PgMembership *,PgGroupKey,int expanded);
void pg_groups_expand_all(PgMembership *,const PgGroups *,int expanded);
#endif
