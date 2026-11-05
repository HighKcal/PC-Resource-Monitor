/* 합성 관계·소속·합계의 경계 시험이다. 행의 단일 소유, 세션 지정, 결측·overflow를
 * 검사하며 실제 OS 부모 생성 사건이나 앱 소속의 정확도를 입증하는 시험은 아니다. */
#include "program_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
static unsigned checks,failures;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; fprintf(stderr,"FAIL line %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
static void row(MonitorRow *r,uint32_t pid)
{
    memset(r,0,sizeof(*r)); r->raw.pid=pid; r->name="fixture.exe"; r->path="SYNTHETIC:\\same.exe";
    r->raw.creation_time_utc_100ns=(uint64_t)pid*100; r->raw.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
    r->raw.cpu_percent=1; r->raw.working_set_bytes=1048576; r->raw.io_read_bytes_per_second=10; r->raw.io_write_bytes_per_second=20;
    r->raw.cpu_interval_seconds=r->raw.io_interval_seconds=3; r->raw.measured_at_utc_100ns=1000000+pid;
}
static void parent(PgProbe *p,const MonitorRow *r)
{
    memset(p,0,sizeof(*p)); p->grade=PG_CURRENT_INFERRED; p->parent_pid_valid=1; p->parent_pid=r->raw.pid; p->parent_creation=r->raw.creation_time_utc_100ns;
}
static void coverage(const MonitorSnapshot *m,const PgGroups *v)
{
    size_t i,j,total=0; unsigned *seen=calloc(m->count?m->count:1,sizeof(*seen)); CHECK(seen!=NULL); if(!seen) return;
    for(i=0;i<v->count;++i) for(j=0;j<v->groups[i].count;++j) { size_t r=v->members[v->groups[i].first+j]; CHECK(r<m->count); if(r<m->count) { ++seen[r]; CHECK(v->owner[r]==i); } ++total; }
    CHECK(total==m->count); for(i=0;i<m->count;++i) CHECK(seen[i]==1); free(seen);
}
static void relations_and_membership(void)
{
    MonitorRow r[8]; PgProbe p[8]; MonitorSnapshot m={0}; PgSnapshot *s=NULL; PgGroups *v=NULL;
    PgTracker *t=pg_tracker_create(); PgMembership *state=pg_membership_create(); size_t i,g; uint64_t target;
    memset(p,0,sizeof(p)); for(i=0;i<8;++i) row(&r[i],(uint32_t)(i+1));
    m.rows=r; m.count=6; m.measured_utc_100ns=2000000;
    parent(&p[1],&r[0]); parent(&p[2],&r[1]); parent(&p[3],&r[0]);
    CHECK(pg_resolve(t,&m,p,&s)); CHECK(s->relations[2].display_parent==1); CHECK(s->inferred==3);
    CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->count==3); CHECK(v->groups[v->owner[0]].count==4); CHECK(v->owner[4]!=v->owner[5]); coverage(&m,v);
    g=v->owner[0]; CHECK(!pg_group_expanded(state,v->groups[g].key)); CHECK(pg_group_set_expanded(state,v->groups[g].key,1)); CHECK(pg_group_expanded(state,v->groups[g].key));
    CHECK(pg_confirm_members(state,v,g,&m,s,1)); target=pg_target_id(state); CHECK(target!=0);
    pg_groups_free(v); v=NULL; CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[0]].key.kind==PG_OWNER_USER);
    CHECK(pg_move_to_target(state,&m,s,4)); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->owner[4]==v->owner[0]); CHECK(v->owner[5]!=v->owner[0]);
    CHECK(pg_separate_shared(state,&m,s,1)); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[1]].key.kind==PG_OWNER_SHARED); CHECK(v->owner[1]!=v->owner[0]); coverage(&m,v);
    CHECK(pg_restore_auto(state,pg_row_identity(&r[1]))); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[1]].key.kind==PG_OWNER_AUTO);
    /* 새 자손은 자동 추정이며 기존 USER 소속을 상속하지 않아야 한다. */
    m.count=7; parent(&p[6],&r[0]); pg_snapshot_free(s); CHECK(pg_resolve(t,&m,p,&s)); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[6]].key.kind==PG_OWNER_AUTO);
    /* 관계 NULL은 개별 목록 대체 표시이며 기존 세션 지정을 초기화하지 않아야 한다. */
    { PgGroups *none=(PgGroups *)1; CHECK(!pg_groups_build(state,&m,NULL,&none)); CHECK(none==NULL); }
    pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[0]].key.kind==PG_OWNER_USER);
    /* 자식 하나를 실행 계통의 나머지 구성원과 독립된 새 이동 대상으로 지정할 수 있어야 한다. */
    CHECK(pg_set_process_target(state,&m,s,6)); CHECK(pg_target_id(state)!=target);
    pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[6]].count==1); CHECK(v->owner[6]!=v->owner[0]);
    target=pg_target_id(state); CHECK(pg_set_process_target(state,&m,s,6)); CHECK(pg_target_id(state)==target);
    /* anchor가 사라져도 남은 지정 구성원은 복사한 그룹 이름과 ID를 유지해야 한다. */
    m.rows=&r[1]; m.count=6; pg_snapshot_free(s); CHECK(pg_resolve(t,&m,&p[1],&s)); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v));
    CHECK(v->groups[v->owner[1]].key.kind==PG_OWNER_USER); CHECK(!strcmp(v->groups[v->owner[1]].label,"fixture.exe"));
    CHECK(s->relations[2].grade==PG_HISTORICAL_INFERRED); CHECK(s->relations[2].display_parent==PG_NONE); coverage(&m,v);
    /* PID가 같아도 생성시각이 바뀌면 이전 지정·펼침을 승계하지 않아야 한다. */
    r[2].raw.creation_time_utc_100ns+=10; pg_snapshot_free(s); CHECK(pg_resolve(t,&m,&p[1],&s)); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v)); CHECK(v->groups[v->owner[1]].key.kind==PG_OWNER_AUTO);
    pg_membership_reset(state); pg_tracker_reset(t); CHECK(pg_target_id(state)==0); pg_snapshot_free(s); CHECK(pg_resolve(t,&m,&p[1],&s)); pg_groups_free(v); CHECK(pg_groups_build(state,&m,s,&v));
    for(i=0;i<v->count;++i) CHECK(v->groups[i].key.kind==PG_OWNER_AUTO);
    pg_groups_free(v); pg_snapshot_free(s); pg_membership_destroy(state); pg_tracker_destroy(t);
}
static void failures_and_bounds(void)
{
    const size_t n=150; MonitorRow *r=calloc(n,sizeof(*r)); PgProbe *p=calloc(n,sizeof(*p)); MonitorSnapshot m={0}; PgTracker *t=pg_tracker_create(); PgMembership *u=pg_membership_create(); PgSnapshot *s=NULL; PgGroups *v=NULL; size_t i;
    CHECK(r && p && t && u); if(!r || !p || !t || !u) goto cleanup;
    for(i=0;i<n;++i) { row(&r[i],(uint32_t)(i+1)); if(i) parent(&p[i],&r[0]); }
    m.rows=r; m.count=n; CHECK(pg_resolve(t,&m,p,&s)); CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->count==1); CHECK(v->groups[0].count==150); coverage(&m,v); pg_groups_free(v); v=NULL; pg_snapshot_free(s); s=NULL;
    r[0].name="explorer.exe"; CHECK(pg_resolve(t,&m,p,&s)); CHECK(s->relations[1].cut==PG_CUT_HOST); CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->count==n); pg_groups_free(v); v=NULL; pg_snapshot_free(s); s=NULL;
    pg_tracker_reset(t); r[0].name="fixture.exe"; for(i=1;i<n;++i) parent(&p[i],&r[i-1]); CHECK(pg_resolve(t,&m,p,&s)); CHECK(s->relations[65].cut==PG_CUT_DEPTH); CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->count==3); coverage(&m,v); pg_groups_free(v); v=NULL; pg_snapshot_free(s); s=NULL;
    /* 잘못된 생성 순서·자기 연결·순환 입력은 연결을 보류하고 원본 행은 잃지 않아야 한다. */
    pg_tracker_reset(t); m.count=4; parent(&p[0],&r[1]); parent(&p[1],&r[0]); parent(&p[2],&r[2]); r[3].raw.valid_fields&=~RESOURCE_VALID_IDENTITY;
    CHECK(pg_resolve(t,&m,p,&s)); CHECK(s->relations[0].display_parent==PG_NONE); CHECK(s->relations[2].cut==PG_CUT_SELF); CHECK(s->relations[3].grade==PG_IDENTITY_UNAVAILABLE);
    CHECK(pg_groups_build(u,&m,s,&v)); coverage(&m,v); CHECK(!pg_set_process_target(u,&m,s,3)); pg_groups_free(v); v=NULL; pg_snapshot_free(s); s=NULL;
    pg_tracker_reset(t); row(&r[3],r[2].raw.pid); CHECK(pg_resolve(t,&m,p,&s)); CHECK(s->relations[2].cut==PG_CUT_DUPLICATE_PID); CHECK(s->relations[3].cut==PG_CUT_DUPLICATE_PID); CHECK(pg_groups_build(u,&m,s,&v)); coverage(&m,v);
cleanup:
    pg_groups_free(v); pg_snapshot_free(s); pg_membership_destroy(u); pg_tracker_destroy(t); free(r); free(p);
}
static void sums(void)
{
    MonitorRow r[3]; PgProbe p[3]={{0}}; MonitorSnapshot m={0}; PgSnapshot *s=NULL; PgGroups *v=NULL; PgMembership *u=pg_membership_create(); PgTracker *t=pg_tracker_create(); size_t i,g;
    for(i=0;i<3;++i) row(&r[i],(uint32_t)(i+1)); parent(&p[1],&r[0]); parent(&p[2],&r[0]); m.rows=r; m.count=3;
    r[0].raw.cpu_percent=0; r[1].raw.cpu_percent=2; r[2].raw.cpu_percent=3; r[1].raw.cpu_interval_seconds=2; r[2].raw.cpu_interval_seconds=4;
    CHECK(pg_resolve(t,&m,p,&s)); CHECK(pg_groups_build(u,&m,s,&v)); g=v->owner[0]; CHECK(v->groups[g].metric[0].value==5); CHECK(v->groups[g].metric[0].valid==3); CHECK(v->groups[g].metric[0].interval_min==2); CHECK(v->groups[g].metric[0].interval_max==4);
    CHECK(v->groups[g].metric[1].bytes==3145728); CHECK(v->groups[g].metric[2].value==30); CHECK(v->groups[g].metric[3].value==60); pg_groups_free(v);
    r[1].raw.cpu_percent=NAN; r[2].raw.cpu_percent=-1; r[1].raw.valid_fields&=~RESOURCE_VALID_MEMORY; r[2].raw.valid_fields&=~RESOURCE_VALID_IO_WRITE;
    CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->groups[0].metric[0].valid==1); CHECK(v->groups[0].metric[0].value==0); CHECK(v->groups[0].metric[1].valid==2); CHECK(v->groups[0].metric[3].valid==2); pg_groups_free(v);
    for(i=0;i<3;++i) r[i].raw.valid_fields=RESOURCE_VALID_IDENTITY;
    CHECK(pg_groups_build(u,&m,s,&v)); for(i=0;i<4;++i) CHECK(v->groups[0].metric[i].valid==0); pg_groups_free(v);
    for(i=0;i<3;++i) { row(&r[i],(uint32_t)(i+1)); r[i].raw.cpu_percent=DBL_MAX; r[i].raw.working_set_bytes=UINT64_MAX; }
    CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->groups[0].metric[0].overflow); CHECK(v->groups[0].metric[1].overflow); pg_groups_free(v);
    r[0].raw.cpu_interval_seconds=0; r[1].raw.cpu_interval_seconds=NAN; r[2].raw.cpu_interval_seconds=-1;
    CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->groups[0].metric[0].valid==0); coverage(&m,v);
    pg_groups_free(v); pg_snapshot_free(s); pg_membership_destroy(u); pg_tracker_destroy(t);
}
static void shell_host_boundary(void)
{
    MonitorRow r[5]; PgProbe p[5]={{0}}; MonitorSnapshot m={0}; PgSnapshot *s=NULL;
    PgGroups *v=NULL; PgTracker *t=pg_tracker_create(); PgMembership *u=pg_membership_create();
    size_t i,j; double direct[4]={0},grouped[4]={0};
    for(i=0;i<5;++i) row(&r[i],(uint32_t)i+1);
    r[0].name="sihost.exe"; r[1].name="first-app.exe"; r[2].name="second-app.exe";
    r[3].name="first-helper.exe"; r[4].name="second-helper.exe";
    parent(&p[1],&r[0]); parent(&p[2],&r[0]); parent(&p[3],&r[1]); parent(&p[4],&r[2]);
    m.rows=r; m.count=5;
    CHECK(pg_resolve(t,&m,p,&s)); CHECK(s->relations[1].cut==PG_CUT_HOST); CHECK(s->relations[2].cut==PG_CUT_HOST);
    CHECK(s->relations[1].grade==PG_CURRENT_INFERRED); CHECK(s->relations[1].raw_parent_pid==r[0].raw.pid);
    CHECK(pg_groups_build(u,&m,s,&v)); CHECK(v->count==3);
    CHECK(v->owner[0]!=v->owner[1] && v->owner[0]!=v->owner[2] && v->owner[1]!=v->owner[2]);
    CHECK(v->owner[1]==v->owner[3]); CHECK(v->owner[2]==v->owner[4]);
    CHECK(v->groups[v->owner[0]].count==1); CHECK(v->groups[v->owner[1]].count==2); CHECK(v->groups[v->owner[2]].count==2);
    coverage(&m,v);
    for(i=0;i<5;++i) { direct[0]+=r[i].raw.cpu_percent; direct[1]+=(double)r[i].raw.working_set_bytes/1048576; direct[2]+=r[i].raw.io_read_bytes_per_second; direct[3]+=r[i].raw.io_write_bytes_per_second; }
    for(i=0;i<v->count;++i) for(j=0;j<4;++j) { grouped[j]+=v->groups[i].metric[j].value; CHECK(v->groups[i].metric[j].valid==v->groups[i].count); }
    for(j=0;j<4;++j) CHECK(direct[j]==grouped[j]);
    pg_groups_free(v); pg_snapshot_free(s); pg_tracker_destroy(t); pg_membership_destroy(u);
}
int main(void)
{
    relations_and_membership(); failures_and_bounds(); sums(); shell_host_boundary();
    printf("grouping checks=%u failures=%u\n",checks,failures); return failures?1:0;
}
