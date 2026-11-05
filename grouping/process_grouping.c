/* OS 조회 결과를 받아 관계·표시 경계를 계산하는 순수 처리부다. 자원 원본은 수정하지 않고
 * 마지막 부모 tuple 관측을 tracker에 보존하며 결과 배열은 독립 소유한다. */
#include "process_grouping.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct PgCache { PgIdentity child, parent; uint64_t seen; } PgCache;
struct PgTracker { PgCache *items; size_t count; };
static int key_compare(PgIdentity a, PgIdentity b)
{
    if (a.pid != b.pid) return a.pid < b.pid ? -1 : 1;
    return a.creation == b.creation ? 0 : a.creation < b.creation ? -1 : 1;
}
static int identity_compare(const void *a, const void *b) { return key_compare(*(const PgIdentity *)a, *(const PgIdentity *)b); }
static int index_compare(const void *a, const void *b)
{
    const PgIndex *x = a, *y = b;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    return x->row == y->row ? 0 : x->row < y->row ? -1 : 1;
}
static int cache_compare(const void *a, const void *b) { return key_compare(((const PgCache *)a)->child, ((const PgCache *)b)->child); }
PgIdentity pg_row_identity(const MonitorRow *r)
{
    PgIdentity k = {0, 0};
    if (r && (r->raw.valid_fields & RESOURCE_VALID_IDENTITY) && r->raw.pid && r->raw.creation_time_utc_100ns) {
        k.pid = r->raw.pid; k.creation = r->raw.creation_time_utc_100ns;
    }
    return k;
}
int pg_identity_equal(PgIdentity a, PgIdentity b) { return a.pid && a.creation && key_compare(a, b) == 0; }
PgTracker *pg_tracker_create(void) { return calloc(1, sizeof(PgTracker)); }
void pg_tracker_reset(PgTracker *t) { if (t) { free(t->items); memset(t, 0, sizeof(*t)); } }
void pg_tracker_destroy(PgTracker *t) { pg_tracker_reset(t); free(t); }
void pg_snapshot_free(PgSnapshot *s) { if (s) { free(s->relations); free(s->order); free(s->index); free(s); } }
static size_t pid_row(const PgSnapshot *s, uint32_t pid)
{
    size_t lo = 0, hi = s->count;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (s->index[m].pid < pid) lo = m + 1; else hi = m; }
    if (lo == s->count || s->index[lo].pid != pid || !s->index[lo].valid) return PG_NONE;
    return s->index[lo].row;
}
size_t pg_find_identity(const PgSnapshot *s, PgIdentity k)
{
    size_t row;
    if (!s || !k.pid || !k.creation) return PG_NONE;
    row = pid_row(s, k.pid);
    if (row != PG_NONE) {
        size_t lo = 0, hi = s->count;
        while (lo < hi) { size_t m = lo + (hi-lo)/2; if (s->index[m].pid < k.pid) lo=m+1; else hi=m; }
        if (s->index[lo].creation == k.creation) return row;
    }
    return PG_NONE;
}
static const PgCache *cache_find(const PgTracker *t, PgIdentity child)
{
    PgCache key; memset(&key, 0, sizeof(key)); key.child = child;
    return t && t->count ? bsearch(&key, t->items, t->count, sizeof(*t->items), cache_compare) : NULL;
}
/* 공용 호스트 아래 서로 다른 앱이 한 후보로 묶이지 않게 표시 연결만 끊는다.
 * 이름 목록은 휴리스틱이며 역할·안전성이나 실제 앱 경계를 증명하지 않는다. */
static int generic_host(const char *name)
{
    static const char *hosts[] = { "System", "smss.exe", "csrss.exe", "wininit.exe", "winlogon.exe", "services.exe", "svchost.exe", "explorer.exe", "sihost.exe" };
    size_t i;
    if (!name) return 0;
    for (i=0; i<sizeof(hosts)/sizeof(hosts[0]); ++i) if (!_stricmp(name, hosts[i])) return 1;
    return 0;
}
/* 이름 정렬이나 경로 병합 없이 원본 행 순서에 따라 형제 연결을 다시 만든다. */
static void adjacency(PgSnapshot *s, size_t *first, size_t *next)
{
    size_t i;
    for (i=0; i<s->count; ++i) first[i] = next[i] = PG_NONE;
    for (i=s->count; i>0; --i) { size_t row=i-1, p=s->relations[row].display_parent; if (p!=PG_NONE) { next[row]=first[p]; first[p]=row; } }
}
static size_t preorder(PgSnapshot *s, const size_t *first, const size_t *next, size_t *stack)
{
    size_t i, used=0, n=0;
    for (i=s->count; i>0; --i) if (s->relations[i-1].display_parent==PG_NONE) stack[n++]=i-1;
    while (n) {
        size_t row=stack[--n], child=first[row], start=n;
        s->order[used++]=row;
        while (child!=PG_NONE) { stack[n++]=child; child=next[child]; }
        /* 스택에서 꺼낼 때 원본 순서가 되도록 형제 구간을 뒤집는다. */
        for (i=start; i<start+(n-start)/2; ++i) { size_t v=stack[i]; stack[i]=stack[n-1-(i-start)]; stack[n-1-(i-start)]=v; }
    }
    return used;
}
int pg_resolve(PgTracker *t, const MonitorSnapshot *m, const PgProbe *probes, PgSnapshot **out)
{
    PgSnapshot *s=NULL;
    PgCache *cache=NULL;
    size_t *scratch=NULL, *first, *next, *stack;
    unsigned char *color=NULL;
    size_t i, n, cache_count=0;
    if (!out) return 0;
    *out=NULL;
    if (!m || m->count > SIZE_MAX / (3*sizeof(size_t)) || m->count > SIZE_MAX/sizeof(PgRelation) || m->count > SIZE_MAX/sizeof(PgIndex)) return 0;
    n=m->count; s=calloc(1,sizeof(*s)); if (!s) return 0;
    s->count=n;
    if (!n) { pg_tracker_reset(t); *out=s; return 1; }
    s->relations=calloc(n,sizeof(*s->relations)); s->order=malloc(n*sizeof(*s->order)); s->index=calloc(n,sizeof(*s->index));
    scratch=malloc(n*3*sizeof(*scratch)); color=calloc(n,1);
    if (!s->relations || !s->order || !s->index || !scratch || !color) goto failed;
    if (t) { cache=malloc((n<PG_MAX_CACHE?n:PG_MAX_CACHE)*sizeof(*cache)); if (!cache) goto failed; }
    first=scratch; next=first+n; stack=next+n;
    for (i=0;i<n;++i) { PgIdentity k=pg_row_identity(&m->rows[i]); s->index[i].pid=m->rows[i].raw.pid; s->index[i].creation=k.creation; s->index[i].row=i; s->index[i].valid=k.pid!=0; }
    qsort(s->index,n,sizeof(*s->index),index_compare);
    for (i=0;i<n;) {
        size_t end=i+1;
        while (end<n && s->index[end].pid==s->index[i].pid) ++end;
        if (end-i>1) { size_t j; for(j=i;j<end;++j) s->index[j].valid=0; }
        i=end;
    }
    for (i=0;i<n;++i) {
        PgRelation *r=&s->relations[i]; PgIdentity child=pg_row_identity(&m->rows[i]);
        const PgProbe *p=probes?&probes[i]:NULL; const PgCache *old;
        size_t parent;
        r->display_parent=PG_NONE; r->grade=p?p->grade:PG_PROBE_FAILED;
        r->raw_parent_pid=p?p->parent_pid:0; r->raw_parent_valid=p?p->parent_pid_valid:0; r->error=p?p->error:0;
        r->cut=PG_CUT_UNVERIFIED;
        if (!child.pid) { r->grade=PG_IDENTITY_UNAVAILABLE; r->cut=PG_CUT_IDENTITY; continue; }
        if (pid_row(s,child.pid)==PG_NONE) { r->cut=PG_CUT_DUPLICATE_PID; continue; }
        old=cache_find(t,child);
        if (p && p->grade==PG_CURRENT_INFERRED && p->parent_pid_valid) {
            r->parent.pid=p->parent_pid; r->parent.creation=p->parent_creation;
            parent=pg_find_identity(s,r->parent);
            if (p->parent_pid==child.pid) r->cut=PG_CUT_SELF;
            else if (!p->parent_creation || p->parent_creation>=child.creation) r->cut=PG_CUT_TIME_ORDER;
            else if (parent==PG_NONE) r->cut=PG_CUT_PARENT_ABSENT;
            else if (old && !pg_identity_equal(old->parent,r->parent)) r->cut=PG_CUT_PARENT_CHANGED;
            else { r->display_parent=parent; r->cut=PG_CUT_NONE; r->last_matched_utc=m->measured_utc_100ns; }
            if (r->display_parent==PG_NONE) r->grade=PG_PID_ONLY;
        }
        /* 현재 검증이 안 되면 같은 child tuple의 과거 부모를 설명용으로만 남긴다.
           과거 부모를 현재 display_parent로 다시 연결하지 않는다. */
        if (r->grade!=PG_CURRENT_INFERRED && old) {
            r->parent=old->parent; r->last_matched_utc=old->seen; r->grade=PG_HISTORICAL_INFERRED;
        }
        if (r->grade==PG_CURRENT_INFERRED || r->grade==PG_HISTORICAL_INFERRED) {
            if (cache && cache_count<PG_MAX_CACHE) { cache[cache_count].child=child; cache[cache_count].parent=r->parent; cache[cache_count++].seen=r->last_matched_utc; }
        }
    }
    /* 유효한 생성시각의 엄격한 순서는 순환을 막지만 입력 방어도 별도로 수행한다.
     * 순환 연결만 끊고 해당 원본 행은 독립 루트로 보존한다. */
    for (i=0;i<n;++i) if (!color[i]) {
        size_t row=i;
        while (row!=PG_NONE && !color[row]) { color[row]=1; row=s->relations[row].display_parent; }
        if (row!=PG_NONE && color[row]==1) {
            size_t cycle=row;
            do { size_t p=s->relations[cycle].display_parent; s->relations[cycle].display_parent=PG_NONE; s->relations[cycle].cut=PG_CUT_CYCLE; cycle=p; } while(cycle!=row);
        }
        row=i; while(row!=PG_NONE && color[row]==1) { color[row]=2; row=s->relations[row].display_parent; }
    }
    for(i=0;i<n;++i) { PgRelation *r=&s->relations[i]; if(r->display_parent!=PG_NONE && generic_host(m->rows[r->display_parent].name)) { r->display_parent=PG_NONE; r->cut=PG_CUT_HOST; } }
    adjacency(s,first,next); if(preorder(s,first,next,stack)!=n) goto failed;
    for(i=0;i<n;++i) {
        size_t row=s->order[i], p=s->relations[row].display_parent;
        PgRelation *r=&s->relations[row];
        if (p!=PG_NONE && s->relations[p].depth+1>PG_MAX_DEPTH) { r->display_parent=PG_NONE; r->cut=PG_CUT_DEPTH; }
        if(r->display_parent==PG_NONE) r->depth=0;
        else r->depth=s->relations[p].depth+1;
    }
    adjacency(s,first,next); if(preorder(s,first,next,stack)!=n) goto failed;
    for(i=n;i>0;--i) {
        size_t row=s->order[i-1], p=s->relations[row].display_parent;
        if(p!=PG_NONE) { ++s->relations[p].children; s->relations[p].descendants+=s->relations[row].descendants+1; }
        else ++s->roots;
        if(s->relations[row].grade==PG_CURRENT_INFERRED) ++s->inferred;
        if(s->relations[row].grade==PG_HISTORICAL_INFERRED) ++s->historical;
    }
    if(t) { qsort(cache,cache_count,sizeof(*cache),cache_compare); free(t->items); t->items=cache; t->count=cache_count; cache=NULL; }
    free(scratch); free(color); *out=s; return 1;
failed:
    free(cache); free(scratch); free(color); pg_snapshot_free(s); return 0;
}
const char *pg_grade_name(PgGrade grade)
{
    switch(grade) {
    case PG_CURRENT_INFERRED:return "OS parent matched (inferred)";
    case PG_HISTORICAL_INFERRED:return "historical OS parent (inferred; not current)";
    case PG_IDENTITY_UNAVAILABLE:return "identity unavailable";
    case PG_PROBE_FAILED:return "parent probe failed";
    case PG_DISABLED:return "grouping disabled";
    default:return "parent PID only / unverified";
    }
}
const char *pg_cut_name(PgCut cut)
{
    switch(cut) {
    case PG_CUT_NONE:return "attached to current inferred parent";
    case PG_CUT_PARENT_ABSENT:return "parent instance absent from current rows";
    case PG_CUT_IDENTITY:return "identity unavailable; independent root";
    case PG_CUT_DUPLICATE_PID:return "duplicate PID; independent root";
    case PG_CUT_SELF:return "self-parent rejected";
    case PG_CUT_TIME_ORDER:return "creation order unverified / rejected";
    case PG_CUT_CYCLE:return "cycle rejected";
    case PG_CUT_HOST:return "generic host display boundary (name heuristic only)";
    case PG_CUT_DEPTH:return "ancestry safety limit (64); estimated scope split";
    case PG_CUT_GROUP_LIMIT:return "scope split";
    case PG_CUT_PARENT_CHANGED:return "cached parent tuple differs; no reattachment";
    default:return "current relationship unverified; independent root";
    }
}
void pg_ui_reset(PgUiState *state) { if(state) { free(state->collapsed); memset(state,0,sizeof(*state)); } }
int pg_ui_collapsed(const PgUiState *state, PgIdentity k)
{
    return state && k.pid && k.creation && state->count && bsearch(&k,state->collapsed,state->count,sizeof(k),identity_compare)!=NULL;
}
int pg_ui_reconcile(PgUiState *state, const PgSnapshot *s)
{
    size_t i,n=0;
    if(!state) return 0;
    for(i=0;i<state->count;++i) if(pg_find_identity(s,state->collapsed[i])!=PG_NONE) state->collapsed[n++]=state->collapsed[i];
    state->count=n; return 1;
}
int pg_ui_set_collapsed(PgUiState *state, PgIdentity k, int collapsed)
{
    size_t i; PgIdentity *items;
    if(!state || !k.pid || !k.creation) return 0;
    for(i=0;i<state->count;++i) if(pg_identity_equal(state->collapsed[i],k)) break;
    if(i<state->count) { if(!collapsed) { memmove(&state->collapsed[i],&state->collapsed[i+1],(state->count-i-1)*sizeof(k)); --state->count; } return 1; }
    if(!collapsed) return 1;
    if(state->count>=PG_MAX_CACHE) return 0;
    items=realloc(state->collapsed,(state->count+1)*sizeof(k)); if(!items) return 0;
    state->collapsed=items; state->collapsed[state->count++]=k;
    qsort(state->collapsed,state->count,sizeof(k),identity_compare); return 1;
}
size_t pg_visible_rows(const PgSnapshot *s, const MonitorSnapshot *m, const PgUiState *state, int flat, size_t *out)
{
    size_t i,n=0;
    if(!m || !out) return 0;
    for(i=0;i<m->count;++i) {
        size_t row=(!flat && s)?s->order[i]:i, p=(!flat && s)?s->relations[row].display_parent:PG_NONE;
        unsigned depth=0; int hidden=0;
        while(p!=PG_NONE && depth++<=PG_MAX_DEPTH) { if(pg_ui_collapsed(state,pg_row_identity(&m->rows[p]))) { hidden=1; break; } p=s->relations[p].display_parent; }
        if(!hidden) out[n++]=row;
    }
    return n;
}
