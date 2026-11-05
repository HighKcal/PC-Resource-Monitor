#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "program_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; printf("FAIL line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)
typedef struct Fixture { MonitorSnapshot m; MonitorRow *rows; PgProbe *probes; } Fixture;
static Fixture fixture(size_t n)
{
    Fixture f; size_t i;
    memset(&f,0,sizeof(f)); f.rows=calloc(n?n:1,sizeof(*f.rows)); f.probes=calloc(n?n:1,sizeof(*f.probes));
    if(!f.rows || !f.probes) exit(2);
    f.m.rows=f.rows; f.m.count=n; f.m.measured_utc_100ns=1000000;
    for(i=0;i<n;++i) {
        MonitorRow *r=&f.rows[i];
        r->name="application.exe"; r->path="C:\\same\\application.exe"; r->program_id=(int64_t)i+1;
        r->raw.pid=(uint32_t)(100+i); r->raw.creation_time_utc_100ns=1000+i;
        r->raw.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
        r->raw.cpu_percent=(double)(i+1); r->raw.working_set_bytes=(uint64_t)(i+1)*10;
        r->raw.io_read_bytes_per_second=(double)i; r->raw.io_write_bytes_per_second=(double)(i+10);
        r->raw.cpu_interval_seconds=1+(double)i; r->raw.io_interval_seconds=2+(double)i;
        r->raw.measured_at_utc_100ns=1000000+i;
        r->metric[MONITOR_CPU].current=9999.0; r->metric[MONITOR_CPU].state=MONITOR_CANDIDATE;
        f.probes[i].grade=PG_PROBE_FAILED;
    }
    return f;
}
static void release_fixture(Fixture *f) { free(f->rows); free(f->probes); memset(f,0,sizeof(*f)); }
static void link_parent(Fixture *f,size_t child,size_t parent)
{
    f->probes[child].grade=PG_CURRENT_INFERRED; f->probes[child].parent_pid_valid=1;
    f->probes[child].parent_pid=f->rows[parent].raw.pid;
    f->probes[child].parent_creation=f->rows[parent].raw.creation_time_utc_100ns;
}
static PgSnapshot *resolve(PgTracker *t,Fixture *f)
{
    PgSnapshot *r=NULL; CHECK(pg_resolve(t,&f->m,f->probes,&r)); CHECK(r!=NULL); return r;
}
static PgGroups *build(PgMembership *s,Fixture *f,PgSnapshot *r)
{
    PgGroups *v=NULL; MonitorRow *before=malloc((f->m.count?f->m.count:1)*sizeof(*before));
    if(!before) exit(2);
    memcpy(before,f->rows,f->m.count*sizeof(*before));
    CHECK(pg_groups_build(s,&f->m,r,&v)); CHECK(v!=NULL);
    CHECK(memcmp(before,f->rows,f->m.count*sizeof(*before))==0); free(before); return v;
}
static void unique_owners(const PgGroups *v,size_t n)
{
    size_t i,j,total=0; unsigned char *seen=calloc(n?n:1,1),*order=calloc(v->count?v->count:1,1);
    if(!seen || !order) exit(2);
    CHECK(v->row_count==n);
    for(i=0;i<v->count;++i) {
        const PgGroup *g=&v->groups[i]; CHECK(g->count>0); CHECK(g->first+g->count<=n);
        for(j=0;j<g->count;++j) { size_t row=v->members[g->first+j]; CHECK(row<n); if(row<n) { CHECK(!seen[row]); seen[row]=1; CHECK(v->owner[row]==i); ++total; } }
        CHECK(v->order[i]<v->count); if(v->order[i]<v->count) { CHECK(!order[v->order[i]]); order[v->order[i]]=1; }
        for(j=0;j<MONITOR_METRICS;++j) { CHECK(g->metric[j].total==g->count); CHECK(g->metric[j].valid<=g->count); }
    }
    CHECK(total==n); for(i=0;i<n;++i) CHECK(seen[i]); free(seen); free(order);
}
static size_t owner_kind_count(const PgGroups *v,PgOwnerKind kind)
{
    size_t i,n=0; for(i=0;i<v->count;++i) if(v->groups[i].key.kind==kind) n+=v->groups[i].count; return n;
}
static void test_sums_and_identity(void)
{
    Fixture f=fixture(6); PgTracker *t=pg_tracker_create(); PgMembership *s=pg_membership_create(); PgSnapshot *r; PgGroups *v; PgGroup *g;
    f.rows[0].name="main.exe"; f.rows[1].name="different-helper.exe";
    link_parent(&f,1,0); link_parent(&f,2,1); link_parent(&f,3,0);
    f.rows[5].raw.valid_fields&=~RESOURCE_VALID_IDENTITY;
    f.rows[0].raw.cpu_percent=10; f.rows[1].raw.cpu_percent=20; f.rows[2].raw.cpu_percent=0; f.rows[3].raw.cpu_percent=NAN;
    f.rows[0].raw.io_write_bytes_per_second=0; f.rows[1].raw.valid_fields&=~RESOURCE_VALID_IO_WRITE;
    f.rows[2].raw.io_write_bytes_per_second=-1; f.rows[3].raw.io_interval_seconds=0;
    r=resolve(t,&f); v=build(s,&f,r); unique_owners(v,6);
    CHECK(v->count==3); CHECK(v->owner[0]==v->owner[1]); CHECK(v->owner[1]==v->owner[2]); CHECK(v->owner[0]!=v->owner[4]);
    g=&v->groups[v->owner[0]]; CHECK(g->count==4); CHECK(g->metric[MONITOR_CPU].value==30); CHECK(g->metric[MONITOR_CPU].valid==3);
    CHECK(g->metric[MONITOR_CPU].interval_min==1); CHECK(g->metric[MONITOR_CPU].interval_max==3);
    CHECK(g->metric[MONITOR_RAM].bytes==100); CHECK(g->metric[MONITOR_RAM].valid==4);
    CHECK(g->metric[MONITOR_READ].value==3); CHECK(g->metric[MONITOR_READ].valid==3);
    CHECK(g->metric[MONITOR_WRITE].value==0); CHECK(g->metric[MONITOR_WRITE].valid==1);
    CHECK(g->observed_min==1000000); CHECK(g->observed_max==1000003);
    CHECK(v->groups[v->owner[5]].metric[MONITOR_RAM].bytes==60);
    CHECK(!pg_confirm_members(s,v,v->owner[5],&f.m,r,1));
    pg_groups_free(v); pg_snapshot_free(r); pg_membership_destroy(s); pg_tracker_destroy(t); release_fixture(&f);
}
static void test_numeric_limits(void)
{
    Fixture f=fixture(3); PgSnapshot *r; PgGroups *v; PgGroup *g; size_t i;
    link_parent(&f,1,0); link_parent(&f,2,0);
    f.rows[0].raw.cpu_percent=DBL_MAX; f.rows[1].raw.cpu_percent=DBL_MAX; f.rows[2].raw.cpu_percent=INFINITY;
    f.rows[0].raw.working_set_bytes=UINT64_MAX; f.rows[1].raw.working_set_bytes=1;
    for(i=0;i<3;++i) { f.rows[i].raw.valid_fields&=~RESOURCE_VALID_IO_READ; f.rows[i].raw.io_write_bytes_per_second=0; }
    r=resolve(NULL,&f); v=build(NULL,&f,r); unique_owners(v,3); g=&v->groups[v->owner[0]];
    CHECK(g->metric[MONITOR_CPU].overflow); CHECK(g->metric[MONITOR_CPU].valid==2);
    CHECK(g->metric[MONITOR_RAM].overflow); CHECK(g->metric[MONITOR_READ].valid==0); CHECK(g->metric[MONITOR_READ].total==3);
    CHECK(g->metric[MONITOR_WRITE].valid==3); CHECK(g->metric[MONITOR_WRITE].value==0);
    pg_groups_free(v); pg_snapshot_free(r); release_fixture(&f);
}
static void test_manual_current_members(void)
{
    Fixture f=fixture(6); PgTracker *t=pg_tracker_create(); PgMembership *s=pg_membership_create(); PgSnapshot *r; PgGroups *v; uint64_t id; PgGroupKey manual; size_t i;
    f.rows[0].name="copied-name.exe"; link_parent(&f,1,0); link_parent(&f,2,1); link_parent(&f,3,0);
    f.m.count=5; r=resolve(t,&f); v=build(s,&f,r);
    CHECK(pg_confirm_members(s,v,v->owner[0],&f.m,r,1)); id=pg_target_id(s); CHECK(id!=0);
    pg_groups_free(v); v=build(s,&f,r); unique_owners(v,5); manual=v->groups[v->owner[0]].key;
    CHECK(manual.kind==PG_OWNER_USER); CHECK(owner_kind_count(v,PG_OWNER_USER)==4);
    CHECK(pg_group_set_expanded(s,manual,1)); CHECK(pg_group_expanded(s,manual));
    pg_groups_free(v);
    /* Metadata OOM is a temporary flat fallback, not a new user session. */
    { PgGroups *fallback=NULL; int ok=pg_groups_build(s,&f.m,NULL,&fallback); CHECK(!ok || fallback!=NULL); pg_groups_free(fallback); }
    v=build(s,&f,r); CHECK(owner_kind_count(v,PG_OWNER_USER)==4); CHECK(pg_group_expanded(s,manual));
    pg_groups_free(v); pg_snapshot_free(r);
    f.m.count=6; link_parent(&f,5,0); r=resolve(t,&f); v=build(s,&f,r); unique_owners(v,6);
    CHECK(owner_kind_count(v,PG_OWNER_USER)==4); CHECK(v->groups[v->owner[5]].key.kind==PG_OWNER_AUTO);
    CHECK(pg_move_to_target(s,&f.m,r,4)); pg_groups_free(v); v=build(s,&f,r); CHECK(owner_kind_count(v,PG_OWNER_USER)==5);
    CHECK(pg_separate_shared(s,&f.m,r,1)); pg_groups_free(v); v=build(s,&f,r); unique_owners(v,6);
    CHECK(owner_kind_count(v,PG_OWNER_SHARED)==1); CHECK(owner_kind_count(v,PG_OWNER_USER)==4);
    CHECK(pg_restore_auto(s,pg_row_identity(&f.rows[1]))); pg_groups_free(v); v=build(s,&f,r); unique_owners(v,6); CHECK(owner_kind_count(v,PG_OWNER_SHARED)==0);
    pg_groups_free(v); pg_snapshot_free(r);
    /* Anchor disappears. Only surviving CURRENT rows contribute, retaining label. */
    for(i=0;i<5;++i) { f.rows[i]=f.rows[i+1]; f.probes[i]=f.probes[i+1]; } f.m.count=5;
    r=resolve(t,&f); v=build(s,&f,r); unique_owners(v,5); i=pg_group_find(v,manual);
    CHECK(i!=PG_NONE); if(i!=PG_NONE) { CHECK(v->groups[i].count==3); CHECK(strcmp(v->groups[i].label,"copied-name.exe")==0); CHECK(v->groups[i].metric[MONITOR_RAM].bytes==120); }
    CHECK(pg_target_id(s)==id);
    pg_groups_free(v); pg_snapshot_free(r);
    /* Same PID, new creation cannot inherit the explicit user assignment. */
    f.rows[1].raw.creation_time_utc_100ns=9000; r=resolve(t,&f); v=build(s,&f,r); unique_owners(v,5);
    CHECK(v->groups[v->owner[1]].key.kind==PG_OWNER_AUTO); CHECK(owner_kind_count(v,PG_OWNER_USER)==2);
    pg_groups_free(v); pg_snapshot_free(r); pg_membership_reset(s); pg_tracker_reset(t);
    r=resolve(t,&f); v=build(s,&f,r); unique_owners(v,5); CHECK(owner_kind_count(v,PG_OWNER_USER)==0); CHECK(pg_target_id(s)==0); CHECK(!pg_group_expanded(s,manual));
    pg_groups_free(v); pg_snapshot_free(r); pg_membership_destroy(s); pg_tracker_destroy(t); release_fixture(&f);
}
static void test_relation_boundaries(void)
{
    Fixture f=fixture(5); PgTracker *t=pg_tracker_create(); PgSnapshot *r; PgGroups *v; uint64_t seen;
    f.rows[0].name="EXPLORER.EXE"; link_parent(&f,1,0); link_parent(&f,2,1); link_parent(&f,3,0);
    r=resolve(t,&f); CHECK(r->relations[1].cut==PG_CUT_HOST); CHECK(r->relations[1].parent.pid==f.rows[0].raw.pid);
    CHECK(r->relations[1].grade==PG_CURRENT_INFERRED); CHECK(r->relations[2].display_parent==1);
    v=build(NULL,&f,r); unique_owners(v,5); CHECK(v->owner[1]==v->owner[2]); CHECK(v->owner[0]!=v->owner[1]); pg_groups_free(v);
    seen=r->relations[2].last_matched_utc; pg_snapshot_free(r);
    f.probes[2].grade=PG_PROBE_FAILED; f.probes[2].error=ERROR_ACCESS_DENIED; ++f.m.measured_utc_100ns;
    r=resolve(t,&f); CHECK(r->relations[2].grade==PG_HISTORICAL_INFERRED); CHECK(r->relations[2].last_matched_utc==seen); CHECK(r->relations[2].display_parent==PG_NONE); CHECK(r->relations[2].error==ERROR_ACCESS_DENIED); pg_snapshot_free(r);
    f.rows[2].raw.creation_time_utc_100ns=9000; r=resolve(t,&f); CHECK(r->relations[2].grade!=PG_HISTORICAL_INFERRED); pg_snapshot_free(r);
    f.rows[3].raw.pid=f.rows[4].raw.pid; link_parent(&f,4,3); link_parent(&f,0,0);
    r=resolve(t,&f); v=build(NULL,&f,r); unique_owners(v,5); CHECK(r->relations[3].display_parent==PG_NONE); CHECK(r->relations[4].display_parent==PG_NONE); CHECK(r->relations[0].display_parent==PG_NONE);
    pg_groups_free(v); pg_snapshot_free(r); pg_tracker_destroy(t); release_fixture(&f);
}
static void test_process_target_scope(void)
{
    Fixture f=fixture(3); PgSnapshot *r; PgGroups *v; PgMembership *s=pg_membership_create(); uint64_t target;
    link_parent(&f,1,0); link_parent(&f,2,0); r=resolve(NULL,&f);
    CHECK(pg_set_process_target(s,&f.m,r,1)); target=pg_target_id(s); CHECK(target!=0);
    v=build(s,&f,r); unique_owners(v,3); CHECK(owner_kind_count(v,PG_OWNER_USER)==1); CHECK(v->groups[v->owner[1]].key.kind==PG_OWNER_USER);
    CHECK(v->groups[v->owner[0]].key.kind==PG_OWNER_AUTO); CHECK(v->groups[v->owner[2]].key.kind==PG_OWNER_AUTO); pg_groups_free(v);
    CHECK(pg_set_process_target(s,&f.m,r,1)); CHECK(pg_target_id(s)==target); CHECK(pg_move_to_target(s,&f.m,r,0));
    v=build(s,&f,r); unique_owners(v,3); CHECK(owner_kind_count(v,PG_OWNER_USER)==2); pg_groups_free(v);
    pg_snapshot_free(r); pg_membership_destroy(s); release_fixture(&f);
}
static void test_shell_host_boundary(void)
{
    Fixture f=fixture(4); PgSnapshot *r; PgGroups *v;
    f.rows[0].name="SIHOST.EXE"; f.rows[1].name="first-app.exe"; f.rows[2].name="second-app.exe"; f.rows[3].name="first-helper.exe";
    link_parent(&f,1,0); link_parent(&f,2,0); link_parent(&f,3,1);
    r=resolve(NULL,&f); v=build(NULL,&f,r); unique_owners(v,4);
    CHECK(r->relations[1].cut==PG_CUT_HOST); CHECK(r->relations[2].cut==PG_CUT_HOST);
    CHECK(r->relations[1].grade==PG_CURRENT_INFERRED); CHECK(r->relations[1].parent.pid==f.rows[0].raw.pid);
    CHECK(r->relations[1].display_parent==PG_NONE); CHECK(r->relations[2].display_parent==PG_NONE); CHECK(r->relations[3].display_parent==1);
    CHECK(v->count==3); CHECK(v->owner[0]!=v->owner[1]); CHECK(v->owner[1]!=v->owner[2]); CHECK(v->owner[1]==v->owner[3]);
    CHECK(v->groups[v->owner[1]].count==2); CHECK(v->groups[v->owner[1]].metric[MONITOR_RAM].bytes==60);
    pg_groups_free(v); pg_snapshot_free(r); release_fixture(&f);
}
static void test_large_and_cancelled(void)
{
    Fixture f=fixture(150); PgTracker *t=pg_tracker_create(); PgSnapshot *r; PgGroups *v; size_t i,n=0; HANDLE stop;
    /* A large browser-like sibling family must not be silently cut at 64. */
    for(i=1;i<150;++i) link_parent(&f,i,0);
    r=resolve(t,&f); v=build(NULL,&f,r); unique_owners(v,150); CHECK(v->count==1); CHECK(v->groups[0].count==150); pg_groups_free(v); pg_snapshot_free(r);
    for(i=1;i<150;++i) link_parent(&f,i,i-1);
    r=resolve(t,&f); v=build(NULL,&f,r); unique_owners(v,150); for(i=0;i<v->count;++i) n+=v->groups[i].count; CHECK(n==150); pg_groups_free(v); pg_snapshot_free(r);
    stop=CreateEventW(NULL,TRUE,TRUE,NULL); CHECK(stop!=NULL); r=NULL;
    CHECK(pg_collect_windows(t,&f.m,stop,&r)); CHECK(r!=NULL);
    if(r) { CHECK(r->probe_error==ERROR_CANCELLED); for(i=0;i<r->count;++i) CHECK(r->relations[i].display_parent==PG_NONE); v=build(NULL,&f,r); unique_owners(v,150); pg_groups_free(v); pg_snapshot_free(r); }
    if(stop) CloseHandle(stop); pg_tracker_destroy(t); release_fixture(&f);
}
int main(void)
{
    test_sums_and_identity(); test_numeric_limits(); test_manual_current_members(); test_relation_boundaries(); test_process_target_scope(); test_shell_host_boundary(); test_large_and_cancelled();
    printf("independent_program_group_review assertions=%u failures=%u\n",assertions,failures); return failures?1:0;
}
