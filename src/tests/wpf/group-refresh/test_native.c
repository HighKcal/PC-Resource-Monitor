/* 그룹 갱신 회귀 전용 DLL이다. 제품 grouping/bridge 본문을 그대로 포함하고,
 * pg_groups_build의 첫 calloc만 지정 횟수만큼 실패시킨다. 생산 DLL에는 넣지 않는다. */
#include <stdlib.h>
#include <string.h>
#include "../../../wpf/native/bridge.h"
#include "../../support/monitor_fixture.h"

static void *group_fixture_calloc(size_t,size_t,unsigned);
static int in_group_build;
static unsigned allocation_ordinal,remaining_failures,allocation_hits,unexpected_allocations,last_line;
static unsigned separate_calls,separate_transitions,build_calls;

#define calloc(n,s) group_fixture_calloc((n),(s),__LINE__)
#define pg_groups_build group_fixture_real_build
#define pg_separate_shared group_fixture_real_separate
#include "../../../grouping/program_groups.c"
#undef pg_separate_shared
#undef pg_groups_build
#undef calloc

static void *group_fixture_calloc(size_t count,size_t bytes,unsigned line)
{
    if(in_group_build && ++allocation_ordinal==1 && remaining_failures) {
        if(count!=1 || bytes!=sizeof(PgGroups)) ++unexpected_allocations;
        else { --remaining_failures; ++allocation_hits; last_line=line; return NULL; }
    }
    return calloc(count,bytes);
}

int pg_groups_build(PgMembership *s,const MonitorSnapshot *m,const PgSnapshot *r,PgGroups **out)
{
    int result;
    ++build_calls; in_group_build=1; allocation_ordinal=0;
    result=group_fixture_real_build(s,m,r,out);
    in_group_build=0;
    return result;
}

/* 호출 수와 성공한 USER/AUTO→SHARED 변경을 따로 센다. 회복 회차가 편집을
 * 재실행하지 않는지는 표시 결과만으로 판단하지 않고 이 계수와 assignment를 대조한다. */
int pg_separate_shared(PgMembership *s,const MonitorSnapshot *m,const PgSnapshot *r,size_t row)
{
    PgIdentity key=pg_row_identity(&m->rows[row]);
    const Assignment *before=assignment_find(s,key),*after;
    int was_shared=before && before->shared;
    int result;
    ++separate_calls;
    result=group_fixture_real_separate(s,m,r,row);
    after=assignment_find(s,key);
    if(result && !was_shared && after && after->shared) ++separate_transitions;
    return result;
}

#include "../../../wpf/native/bridge.c"

typedef struct GroupFixtureObservation {
    int has_assignment,shared,shown_kind,pending,failed,grouping_failed;
    unsigned allocation_hits,remaining_failures,unexpected_allocations,last_line;
    unsigned separate_calls,separate_transitions,build_calls,session;
    uint64_t assignment_id,target_id,sequence,shown_id,assigned_count,shown_count;
} GroupFixtureObservation;

/* 관측과 arm은 bridge를 소유하는 worker에서만 호출한다. 반환 구조체는 값 사본이다. */
BRIDGE_API int group_fixture_observe(Bridge *b,GroupFixtureObservation *out)
{
    const Assignment *a=NULL;
    const PgGroup *g=NULL;
    if(!worker(b) || !out) return 0;
    memset(out,0,sizeof(*out));
    if(b->snapshot && b->snapshot->count) a=assignment_find(b->membership,pg_row_identity(&b->snapshot->rows[0]));
    if(b->groups && b->groups->count) g=&b->groups->groups[0];
    out->has_assignment=a!=NULL; out->shared=a?a->shared:-1; out->shown_kind=g?(int)g->key.kind:-1;
    out->pending=bridge_group_refresh_pending(b); out->failed=b->failed; out->grouping_failed=b->view.grouping_failed;
    out->allocation_hits=allocation_hits; out->remaining_failures=remaining_failures;
    out->unexpected_allocations=unexpected_allocations; out->last_line=last_line;
    out->separate_calls=separate_calls; out->separate_transitions=separate_transitions; out->build_calls=build_calls;
    out->session=b->session; out->assignment_id=a?a->id:0; out->target_id=pg_target_id(b->membership);
    out->sequence=b->snapshot?b->snapshot->sequence:0; out->shown_id=g?g->key.id:0;
    out->assigned_count=b->membership->assigned; out->shown_count=b->groups?b->groups->count:0;
    return 1;
}

BRIDGE_API int group_fixture_arm(Bridge *b,unsigned failures,int reset_counts)
{
    if(!worker(b)) return 0;
    remaining_failures=failures;
    if(reset_counts) {
        allocation_hits=unexpected_allocations=last_line=0;
        separate_calls=separate_transitions=build_calls=0;
    }
    return 1;
}

/* native NULL/view 반환 자체를 보는 대조다. 정상 UI 검사는 Engine.Edit를 사용한다. */
BRIDGE_API int group_fixture_edit(Bridge *b,int action,int stale_session)
{
    PgIdentity key;
    if(!worker(b) || !b->snapshot || b->snapshot->count!=1) return -1;
    key=pg_row_identity(&b->snapshot->rows[0]);
    return bridge_edit(b,b->session+(stale_session?1u:0u),action,key.pid,key.creation,(PgGroupKey){0})!=NULL;
}

/* 기존 test support의 DB 핸들만 빌린다. 호출자는 이 fixture의 새 합성 DB에만
 * trigger를 설치하며 실행 SQL/실패 경계는 managed 원장에 남긴다. */
BRIDGE_API int group_fixture_sql(Bridge *b,const char *sql)
{
    if(!worker(b) || !b->engine || !sql) return SQLITE_MISUSE;
    return sqlite3_exec(monitor_fixture_db(b->engine),sql,NULL,NULL,NULL);
}
