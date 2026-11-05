#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monitor.h"
#include "program_groups.h"
#include "../../gui/native_availability.h"

static unsigned checks,failures;
#define CHECK(c) do { ++checks; if(!(c)) { ++failures; printf("FAIL line %u: %s\n",(unsigned)__LINE__,#c); } } while(0)

static void test_metadata_and_names(void)
{
    MonitorRow rows[2]={0},before[2]; MonitorSnapshot snapshot={0};
    ResourceDisplayInfo infos[2]={0}; ResourceDisplayBatch display={0};
    const ResourceDisplayInfo *info;
    snapshot.rows=rows; snapshot.count=2; display.items=infos; display.count=2;
    rows[0].name="(unavailable)"; rows[0].raw.pid=71; rows[0].raw.measured_at_utc_100ns=900;
    rows[1].name="verified.exe"; rows[1].raw.process_name_utf8=rows[1].name;
    rows[1].raw.pid=72; rows[1].raw.measured_at_utc_100ns=901;
    rows[1].raw.valid_fields=RESOURCE_VALID_NAME|RESOURCE_VALID_IDENTITY; rows[1].raw.creation_time_utc_100ns=100;
    infos[0].pid=71; infos[0].measured_at_utc_100ns=900; infos[0].name_source=RESOURCE_NAME_LIST; infos[0].snapshot_name_utf8="observed.exe";
    infos[1].pid=72; infos[1].measured_at_utc_100ns=901; infos[1].creation_time_utc_100ns=100; infos[1].name_source=RESOURCE_NAME_HANDLE;
    memcpy(before,rows,sizeof(rows));
    info=ui_display_metadata(&display,&snapshot,0); CHECK(info==&infos[0]);
    CHECK(ui_name_source(&rows[0],info)==RESOURCE_NAME_LIST); CHECK(!strcmp(ui_name_text(&rows[0],info),"observed.exe"));
    CHECK(strstr(ui_name_source_text(RESOURCE_NAME_LIST),"참고")!=NULL);
    CHECK(ui_name_source(&rows[1],ui_display_metadata(&display,&snapshot,1))==RESOURCE_NAME_HANDLE);
    CHECK(!strcmp(ui_name_text(&rows[1],NULL),"verified.exe"));
    CHECK(strstr(ui_name_source_text(RESOURCE_NAME_HANDLE),"핸들")!=NULL);
    CHECK(ui_name_source(&rows[0],NULL)==RESOURCE_NAME_UNAVAILABLE);
    CHECK(strstr(ui_name_text(&rows[0],NULL),"확인 불가")!=NULL);
    CHECK(strstr(ui_name_source_text(RESOURCE_NAME_UNAVAILABLE),"PID")!=NULL);
    CHECK(!memcmp(before,rows,sizeof(rows))); /* No validity/history promotion. */
    CHECK(ui_display_metadata(&display,&snapshot,2)==NULL);
    display.count=1; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); display.count=2;
    infos[0].pid=72; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); infos[0].pid=71;
    ++infos[0].measured_at_utc_100ns; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); --infos[0].measured_at_utc_100ns;
    infos[0].creation_time_utc_100ns=100; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); infos[0].creation_time_utc_100ns=0;
    ++infos[1].creation_time_utc_100ns; CHECK(ui_display_metadata(&display,&snapshot,1)==NULL); --infos[1].creation_time_utc_100ns;
    rows[0].raw.measured_at_utc_100ns=902; /* Same PID in a later observation: no reuse. */
    CHECK(ui_display_metadata(&display,&snapshot,0)==NULL);
    CHECK(ui_name_source(&rows[0],ui_display_metadata(&display,&snapshot,0))==RESOURCE_NAME_UNAVAILABLE);
    rows[0].raw.process_name_utf8="old.exe"; CHECK(ui_name_source(&rows[0],NULL)==RESOURCE_NAME_UNAVAILABLE);
    rows[1].raw.valid_fields&=~RESOURCE_VALID_NAME; CHECK(ui_name_source(&rows[1],&infos[1])==RESOURCE_NAME_UNAVAILABLE);
    infos[1].name_source=RESOURCE_NAME_LIST; infos[1].snapshot_name_utf8=""; CHECK(ui_name_source(&rows[1],&infos[1])==RESOURCE_NAME_UNAVAILABLE);
    CHECK(ui_display_metadata(NULL,&snapshot,0)==NULL); CHECK(ui_display_metadata(&display,NULL,0)==NULL);
}
static void test_failure_explanations(void)
{
    MonitorRow row={0}; ResourceDisplayInfo info={0}; const char *reason;
    row.raw.open_error=ERROR_ACCESS_DENIED;
    reason=ui_field_query_reason(&row,&info,0); CHECK(reason && strstr(reason,"후속 조회")!=NULL);
    CHECK(strstr(ui_identity_reason(&row,&info),"접근 권한")!=NULL);
    CHECK(strstr(ui_path_reason(&row),"실행하지 않았")!=NULL);
    row.raw.open_error=ERROR_INVALID_PARAMETER;
    reason=ui_field_query_reason(&row,&info,0); CHECK(reason && strstr(reason,"종료 여부는 확인되지")!=NULL);
    row.raw.open_error=0; info.exit_observed=1;
    CHECK(strstr(ui_field_query_reason(&row,&info,ERROR_ACCESS_DENIED),"종료가 확인")!=NULL);
    CHECK(ui_field_query_reason(&row,&info,0)==NULL); /* Other field failure/exit must not hide first-sample delta. */
    info.exit_observed=0;
    CHECK(strstr(ui_field_query_reason(&row,&info,ERROR_ACCESS_DENIED),"접근 권한")!=NULL);
    CHECK(strstr(ui_field_query_reason(&row,&info,ERROR_GEN_FAILURE),"조회하지 못했")!=NULL);
    CHECK(ui_field_query_reason(&row,&info,0)==NULL); /* First sample belongs to delta status. */
    CHECK(strstr(ui_identity_reason(&row,&info),"확인할 수 없")!=NULL);
    row.raw.valid_fields=RESOURCE_VALID_IDENTITY; row.raw.creation_time_utc_100ns=100;
    CHECK(strstr(ui_identity_reason(&row,&info),"생성시각 확인됨")!=NULL);
    row.path_error=ERROR_ACCESS_DENIED; CHECK(strstr(ui_path_reason(&row),"접근 권한")!=NULL);
    row.path_error=ERROR_INVALID_DATA; CHECK(strstr(ui_path_reason(&row),"생성시각이 달라")!=NULL);
    row.path_error=ERROR_GEN_FAILURE; CHECK(strstr(ui_path_reason(&row),"학습과 비교에서 제외")!=NULL);
    row.path="C:\\verified.exe"; CHECK(strstr(ui_path_reason(&row),"생성시각을 대조")!=NULL);
}
static void test_same_name_grouping(void)
{
    MonitorRow rows[2]={0}; MonitorSnapshot snapshot={0}; PgSnapshot *relations=NULL; PgGroups *groups=NULL;
    PgMembership *membership=pg_membership_create(); PgProbe probes[2]={0}; unsigned i;
    snapshot.rows=rows; snapshot.count=2; CHECK(membership!=NULL);
    for(i=0;i<2;++i) {
        rows[i].name="same.exe"; rows[i].raw.process_name_utf8=rows[i].name;
        rows[i].raw.pid=200+i; rows[i].raw.creation_time_utc_100ns=100+i;
        rows[i].raw.valid_fields=RESOURCE_VALID_IDENTITY; probes[i].grade=PG_PROBE_FAILED;
    }
    CHECK(pg_resolve(NULL,&snapshot,probes,&relations)); CHECK(pg_groups_build(membership,&snapshot,relations,&groups));
    CHECK(groups && groups->count==2); CHECK(groups && groups->owner[0]!=groups->owner[1]);
    pg_groups_free(groups); groups=NULL; pg_snapshot_free(relations); relations=NULL;
    for(i=0;i<2;++i) rows[i].raw.valid_fields=0;
    CHECK(pg_resolve(NULL,&snapshot,probes,&relations)); CHECK(pg_groups_build(membership,&snapshot,relations,&groups));
    CHECK(groups && groups->count==2); CHECK(groups && groups->owner[0]!=groups->owner[1]);
    CHECK(groups && groups->groups[0].metric[MONITOR_RAM].valid==0); /* Display names never create a zero measurement. */
    pg_groups_free(groups); pg_snapshot_free(relations); pg_membership_destroy(membership);
}
int main(void)
{
    test_metadata_and_names(); test_failure_explanations(); test_same_name_grouping();
    printf("UI availability: %u checks, %u failures\n",checks,failures); return failures?1:0;
}
