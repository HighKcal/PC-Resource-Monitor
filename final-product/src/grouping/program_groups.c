/* 표시 세션의 소속과 현재 원시 자원 합계를 만든다. pg_groups_build에서 소유 그룹
 * 선택 → 같은 그룹끼리 정렬 → 필드별 합산 → 표시 순서를 읽는다. 자동 관계 후보 위에
 * 사용자 지정을 적용하되 한 행을 합계 한 곳에만 넣고 DB·Baseline·완료창은 바꾸지 않는다. */
#include "program_groups.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

typedef struct Assignment { PgIdentity key; uint64_t id; int shared; } Assignment;
typedef struct Definition { uint64_t id; char label[256]; int used; } Definition;
struct PgMembership {
    Assignment *assignments; size_t assigned;
    Definition *definitions; size_t defined;
    PgGroupKey *expanded; size_t expansions;
    uint64_t next_id,target;
};
typedef struct OwnerRow { PgGroupKey key; size_t row; PgIdentity process; } OwnerRow;
typedef struct GroupOrder { size_t index; int rank; PgGroupKey key; } GroupOrder;
static int identity_cmp(PgIdentity a,PgIdentity b)
{
    if(a.pid!=b.pid) return a.pid<b.pid?-1:1;
    return a.creation==b.creation?0:a.creation<b.creation?-1:1;
}
static int group_cmp(PgGroupKey a,PgGroupKey b)
{
    if(a.kind!=b.kind) return a.kind<b.kind?-1:1;
    if(a.id!=b.id) return a.id<b.id?-1:1;
    return identity_cmp(a.anchor,b.anchor);
}
int pg_group_key_equal(PgGroupKey a,PgGroupKey b) { return group_cmp(a,b)==0; }
static int owner_cmp(const void *a,const void *b)
{
    const OwnerRow *x=a,*y=b; int c=group_cmp(x->key,y->key);
    if(c) return c;
    c=identity_cmp(x->process,y->process); if(c) return c;
    return x->row==y->row?0:x->row<y->row?-1:1;
}
static int order_cmp(const void *a,const void *b) { const GroupOrder *x=a,*y=b; return x->rank==y->rank?group_cmp(x->key,y->key):x->rank<y->rank?-1:1; }
static int assignment_cmp(const void *a,const void *b) { return identity_cmp(((const Assignment *)a)->key,((const Assignment *)b)->key); }
static const Assignment *assignment_find(const PgMembership *s,PgIdentity key)
{
    Assignment v; memset(&v,0,sizeof(v)); v.key=key;
    return s && s->assigned?bsearch(&v,s->assignments,s->assigned,sizeof(v),assignment_cmp):NULL;
}
static const Definition *definition_find(const PgMembership *s,uint64_t id)
{
    size_t lo=0,hi=s?s->defined:0;
    while(lo<hi) { size_t mid=lo+(hi-lo)/2; if(s->definitions[mid].id<id) lo=mid+1; else hi=mid; }
    return s && lo<s->defined && s->definitions[lo].id==id?&s->definitions[lo]:NULL;
}
PgMembership *pg_membership_create(void) { return calloc(1,sizeof(PgMembership)); }
void pg_membership_reset(PgMembership *s) { if(s) { free(s->assignments); free(s->definitions); free(s->expanded); memset(s,0,sizeof(*s)); } }
void pg_membership_destroy(PgMembership *s) { pg_membership_reset(s); free(s); }
void pg_groups_free(PgGroups *v) { if(v) { free(v->groups); free(v->members); free(v->owner); free(v->order); free(v); } }
size_t pg_group_find(const PgGroups *v,PgGroupKey key)
{
    size_t lo=0,hi=v?v->count:0;
    while(lo<hi) { size_t mid=lo+(hi-lo)/2; if(group_cmp(v->groups[mid].key,key)<0) lo=mid+1; else hi=mid; }
    return v && lo<v->count && !group_cmp(v->groups[lo].key,key)?lo:PG_NONE;
}
static void prune(PgMembership *s,const PgSnapshot *relations)
{
    size_t i,n=0;
    if(!s) return;
    for(i=0;i<s->assigned;++i) if(pg_find_identity(relations,s->assignments[i].key)!=PG_NONE) s->assignments[n++]=s->assignments[i];
    s->assigned=n;
    for(i=0;i<s->defined;++i) s->definitions[i].used=s->definitions[i].id==s->target;
    for(i=0;i<s->assigned;++i) if(!s->assignments[i].shared) {
        const Definition *d=definition_find(s,s->assignments[i].id);
        if(d) s->definitions[d-s->definitions].used=1;
    }
    n=0; for(i=0;i<s->defined;++i) if(s->definitions[i].used) s->definitions[n++]=s->definitions[i];
    s->defined=n;
}
/* 결측은 합에 넣지 않고 valid/total을 따로 센다. CPU·I/O는 유한한 비음수 값과
 * 양수 관측초가 필요하며 서로 다른 관측구간의 최신율 합이다. RAM은 Working Set bytes
 * 합으로 공유 페이지가 중복될 수 있다. overflow가 나면 부분 숫자를 정상 합계로 쓰지 않는다. */
static void add_sum(PgSum *sum,const ResourceData *r,unsigned metric)
{
    unsigned bit=metric==MONITOR_CPU?RESOURCE_VALID_CPU:metric==MONITOR_RAM?RESOURCE_VALID_MEMORY:metric==MONITOR_READ?RESOURCE_VALID_IO_READ:RESOURCE_VALID_IO_WRITE;
    double value=0,interval=metric==MONITOR_CPU?r->cpu_interval_seconds:r->io_interval_seconds;
    ++sum->total;
    if(!(r->valid_fields&bit)) return;
    if(metric==MONITOR_RAM) {
        ++sum->valid;
        if(UINT64_MAX-sum->bytes<r->working_set_bytes) sum->overflow=1;
        else if(!sum->overflow) sum->bytes+=r->working_set_bytes;
        /* RAM value는 기존 내부 MiB 환산값이다. GUI의 decimal GB 표시는 bytes를 사용한다. */
        sum->value=(double)sum->bytes/1048576.0; return;
    }
    value=metric==MONITOR_CPU?r->cpu_percent:metric==MONITOR_READ?r->io_read_bytes_per_second:r->io_write_bytes_per_second;
    if(!isfinite(value) || value<0 || !isfinite(interval) || interval<=0) return;
    ++sum->valid;
    if(sum->valid==1 || interval<sum->interval_min) sum->interval_min=interval;
    if(interval>sum->interval_max) sum->interval_max=interval;
    if(value>DBL_MAX-sum->value) sum->overflow=1;
    else if(!sum->overflow) sum->value+=value;
}
/* 현재 신원이 유일한 행에만 세션 지정을 적용한다. 지정이 없으면 resolver가 이미
 * 끊어 놓은 표시 경계 안에서 루트를 찾는다. 신원 미확인·중복 PID는 이 snapshot에서만
 * 유효한 행별 그룹으로 남겨 다른 인스턴스나 다음 회차에 소속이 승계되지 않게 한다. */
static OwnerRow owner_for_row(const PgMembership *state,const MonitorSnapshot *metrics,
    const PgSnapshot *relations,size_t row)
{
    OwnerRow owner={0};
    PgIdentity key=pg_row_identity(&metrics->rows[row]);
    const Assignment *assignment=NULL;
    size_t root=row;
    unsigned depth=0;
    if(pg_find_identity(relations,key)==row) assignment=assignment_find(state,key);
    owner.row=row; owner.process=key;
    if(assignment) {
        owner.key.kind=assignment->shared?PG_OWNER_SHARED:PG_OWNER_USER;
        owner.key.id=assignment->id;
        if(assignment->shared) owner.key.anchor=key;
    } else {
        while(relations->relations[root].display_parent!=PG_NONE && depth++<=PG_MAX_DEPTH)
            root=relations->relations[root].display_parent;
        owner.key.kind=PG_OWNER_AUTO; owner.key.anchor=pg_row_identity(&metrics->rows[root]);
        if(pg_find_identity(relations,owner.key.anchor)!=root) {
            owner.key.anchor.pid=0; owner.key.anchor.creation=0; owner.key.id=(uint64_t)row+1;
        }
    }
    return owner;
}

static void begin_group(PgGroup *group,const OwnerRow *owner,size_t first,const PgMembership *state,
    const MonitorSnapshot *metrics,const PgSnapshot *relations)
{
    const Definition *definition;
    group->key=owner->key; group->first=first;
    definition=definition_find(state,group->key.id);
    if(group->key.kind==PG_OWNER_USER && definition)
        snprintf(group->label,sizeof(group->label),"%s",definition->label);
    else {
        size_t anchor=pg_find_identity(relations,group->key.anchor);
        const char *name=metrics->rows[anchor==PG_NONE?owner->row:anchor].name;
        snprintf(group->label,sizeof(group->label),"%s",name?name:"unknown process");
    }
}

/* 정렬된 한 행을 정확히 한 그룹에 연결한 뒤 현재 원시값만 합산한다. 완료창의
 * 개인화 판정은 그룹 합계로 올리지 않으며 결측과 overflow는 add_sum에 남긴다. */
static void append_group_member(PgGroups *view,size_t member,size_t row,
    const MonitorSnapshot *metrics,const PgSnapshot *relations)
{
    PgGroup *group=&view->groups[view->count-1];
    const MonitorRow *source=&metrics->rows[row];
    unsigned metric;
    ++group->count; view->members[member]=row; view->owner[row]=view->count-1;
    for(metric=0;metric<MONITOR_METRICS;++metric) add_sum(&group->metric[metric],&source->raw,metric);
    if(!group->observed_min || source->raw.measured_at_utc_100ns<group->observed_min)
        group->observed_min=source->raw.measured_at_utc_100ns;
    if(source->raw.measured_at_utc_100ns>group->observed_max) group->observed_max=source->raw.measured_at_utc_100ns;
    if(relations->relations[row].cut==PG_CUT_DEPTH) ++group->scope_splits;
    if(relations->relations[row].grade==PG_HISTORICAL_INFERRED) ++group->historical;
}

/* 입력 배열은 빌리고, 할당이 모두 성공한 뒤에만 세션 지정의 부재 항목을 정리한다.
 * 실패는 *out=NULL이며 임시 배열을 모두 해제한다. 성공 출력은 호출자가 소유한다. */
int pg_groups_build(PgMembership *state,const MonitorSnapshot *metrics,const PgSnapshot *relations,PgGroups **out)
{
    PgGroups *v=NULL; OwnerRow *rows=NULL; GroupOrder *ordering=NULL; size_t i,n;
    if(!out) return 0;
    *out=NULL;
    if(!metrics || !relations || relations->count!=metrics->count) return 0;
    n=metrics->count;
    if(n>SIZE_MAX/sizeof(*rows) || n>SIZE_MAX/sizeof(PgGroup)) return 0;
    v=calloc(1,sizeof(*v)); if(!v) return 0; v->row_count=n;
    if(!n) { prune(state,relations); *out=v; return 1; }
    rows=calloc(n,sizeof(*rows)); ordering=calloc(n,sizeof(*ordering)); v->groups=calloc(n,sizeof(*v->groups)); v->members=malloc(n*sizeof(*v->members)); v->owner=malloc(n*sizeof(*v->owner)); v->order=malloc(n*sizeof(*v->order));
    if(!rows || !ordering || !v->groups || !v->members || !v->owner || !v->order) goto failed;
    prune(state,relations);
    for(i=0;i<n;++i) rows[i]=owner_for_row(state,metrics,relations,i);
    qsort(rows,n,sizeof(*rows),owner_cmp);
    for(i=0;i<n;++i) {
        if(!i || !pg_group_key_equal(rows[i-1].key,rows[i].key))
            begin_group(&v->groups[v->count++],&rows[i],i,state,metrics,relations);
        append_group_member(v,i,rows[i].row,metrics,relations);
    }
    for(i=0;i<v->count;++i) { ordering[i].index=i; ordering[i].key=v->groups[i].key; ordering[i].rank=v->groups[i].count>1?0:v->groups[i].key.anchor.pid || v->groups[i].key.kind==PG_OWNER_USER?1:2; }
    qsort(ordering,v->count,sizeof(*ordering),order_cmp); for(i=0;i<v->count;++i) v->order[i]=ordering[i].index;
    if(state) { size_t used=0; for(i=0;i<state->expansions;++i) if(pg_group_find(v,state->expanded[i])!=PG_NONE) state->expanded[used++]=state->expanded[i]; state->expansions=used; }
    free(rows); free(ordering); *out=v; return 1;
failed:
    free(rows); free(ordering); pg_groups_free(v); return 0;
}
/* 새 배열의 할당·복사·정렬이 끝난 뒤 교체해 실패 시 기존 지정 내용은 보존한다.
 * 현재 tuple만 기록하므로 나중에 생긴 자식이나 재사용 PID에 지정이 상속되지 않는다. */
static int assign_many(PgMembership *s,const PgIdentity *keys,size_t n,uint64_t id,int shared)
{
    Assignment *items; size_t i,used=s->assigned;
    if(n>PG_MAX_CACHE || s->assigned>PG_MAX_CACHE-n) {
        size_t existing=0;
        for(i=0;i<n;++i) if(assignment_find(s,keys[i])) ++existing;
        if(n-existing>PG_MAX_CACHE-s->assigned) return 0;
    }
    if(s->assigned+n>SIZE_MAX/sizeof(*items)) return 0;
    items=malloc((s->assigned+n)*sizeof(*items)); if(!items && s->assigned+n) return 0;
    if(s->assigned) memcpy(items,s->assignments,s->assigned*sizeof(*items));
    for(i=0;i<n;++i) {
        const Assignment *old=assignment_find(s,keys[i]); size_t position=old?(size_t)(old-s->assignments):used++;
        items[position].key=keys[i]; items[position].id=id; items[position].shared=shared;
    }
    qsort(items,used,sizeof(*items),assignment_cmp); free(s->assignments); s->assignments=items; s->assigned=used; return 1;
}
/* GUI의 그룹 확정 명령이다. 호출자는 최신 소속으로 만든 목록을 전달해야 한다.
 * 기존 USER 그룹은 지정된 ID를 그대로 선택하고, 새 그룹은 구성원 신원을 재확인한다.
 * 새 그룹은 assign_many 성공 전에는 지정 내용과 target을 바꾸지 않는다. 표시 합계 재구성은
 * 호출자가 별도로 수행하므로 이 성공이 다음 pg_groups_build의 성공까지 보장하지 않는다. */
int pg_confirm_members(PgMembership *s,const PgGroups *v,size_t group,const MonitorSnapshot *m,const PgSnapshot *r,int set_target)
{
    const PgGroup *g; PgIdentity *keys; Definition *defs=NULL; uint64_t id; size_t i;
    if(!s || !v || group>=v->count || !m || !r) return 0;
    g=&v->groups[group];
    if(g->key.kind==PG_OWNER_USER) { if(set_target) s->target=g->key.id; return 1; }
    if(g->count>PG_MAX_CACHE || s->defined>=PG_MAX_CACHE || s->next_id==UINT64_MAX) return 0;
    keys=malloc(g->count*sizeof(*keys)); if(!keys && g->count) return 0;
    for(i=0;i<g->count;++i) { size_t row=v->members[g->first+i]; keys[i]=pg_row_identity(&m->rows[row]); if(pg_find_identity(r,keys[i])!=row) { free(keys); return 0; } }
    defs=realloc(s->definitions,(s->defined+1)*sizeof(*defs)); if(!defs) { free(keys); return 0; }
    s->definitions=defs; id=s->next_id+1;
    if(!assign_many(s,keys,g->count,id,0)) { free(keys); return 0; }
    s->next_id=id; defs[s->defined].id=id; snprintf(defs[s->defined].label,sizeof(defs[s->defined].label),"%s",g->label); ++s->defined;
    if(set_target) s->target=id;
    free(keys); return 1;
}
int pg_set_process_target(PgMembership *s,const MonitorSnapshot *m,const PgSnapshot *r,size_t row)
{
    PgIdentity key; const Assignment *a; PgGroup group; PgGroups view;
    if(!s || !m || !r || row>=m->count) return 0;
    key=pg_row_identity(&m->rows[row]); if(pg_find_identity(r,key)!=row) return 0;
    a=assignment_find(s,key);
    if(a && !a->shared && definition_find(s,a->id)) { s->target=a->id; return 1; }
    memset(&group,0,sizeof(group)); memset(&view,0,sizeof(view));
    group.count=1; group.key.anchor=key; snprintf(group.label,sizeof(group.label),"%s",m->rows[row].name?m->rows[row].name:"unknown process");
    view.groups=&group; view.count=1; view.members=&row;
    return pg_confirm_members(s,&view,0,m,r,1);
}
static int assign_one(PgMembership *s,const MonitorSnapshot *m,const PgSnapshot *r,size_t row,int shared)
{
    PgIdentity key;
    if(!s || !m || !r || row>=m->count || (!shared && !definition_find(s,s->target))) return 0;
    key=pg_row_identity(&m->rows[row]); if(pg_find_identity(r,key)!=row) return 0;
    return assign_many(s,&key,1,shared?0:s->target,shared);
}
int pg_move_to_target(PgMembership *s,const MonitorSnapshot *m,const PgSnapshot *r,size_t row) { return assign_one(s,m,r,row,0); }
int pg_separate_shared(PgMembership *s,const MonitorSnapshot *m,const PgSnapshot *r,size_t row) { return assign_one(s,m,r,row,1); }
int pg_restore_auto(PgMembership *s,PgIdentity key)
{
    const Assignment *a; size_t i;
    if(!s || !key.pid || !key.creation) return 0;
    a=assignment_find(s,key); if(!a) return 1; i=(size_t)(a-s->assignments);
    memmove(&s->assignments[i],&s->assignments[i+1],(s->assigned-i-1)*sizeof(*a)); --s->assigned; return 1;
}
uint64_t pg_target_id(const PgMembership *s) { return s?s->target:0; }
const char *pg_target_label(const PgMembership *s) { const Definition *d=s?definition_find(s,s->target):NULL; return d?d->label:"none"; }
int pg_group_expanded(const PgMembership *s,PgGroupKey key)
{
    size_t i; if(s) for(i=0;i<s->expansions;++i) if(pg_group_key_equal(s->expanded[i],key)) return 1; return 0;
}
int pg_group_set_expanded(PgMembership *s,PgGroupKey key,int expanded)
{
    size_t i; PgGroupKey *keys;
    if(!s || (key.kind==PG_OWNER_AUTO && (!key.anchor.pid || !key.anchor.creation))) return 0;
    for(i=0;i<s->expansions;++i) if(pg_group_key_equal(s->expanded[i],key)) break;
    if(i<s->expansions) { if(!expanded) { memmove(&s->expanded[i],&s->expanded[i+1],(s->expansions-i-1)*sizeof(key)); --s->expansions; } return 1; }
    if(!expanded) return 1;
    if(s->expansions>=PG_MAX_CACHE) return 0;
    keys=realloc(s->expanded,(s->expansions+1)*sizeof(key)); if(!keys) return 0;
    s->expanded=keys; s->expanded[s->expansions++]=key; return 1;
}
void pg_groups_expand_all(PgMembership *s,const PgGroups *v,int expanded)
{
    size_t i; if(!s || !v) return;
    if(!expanded) { s->expansions=0; return; }
    for(i=0;i<v->count;++i) if(v->groups[i].count>1) pg_group_set_expanded(s,v->groups[i].key,1);
}
