#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "program_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Reads only marker-named fixture processes; collecting twice verifies real
 * QPC fields. --parent-exit writes the fixture's normal-exit request marker. */
typedef struct Fixture { uint32_t pid; unsigned role; uint64_t creation; } Fixture;
static int read_markers(const wchar_t *directory,Fixture *fixtures,size_t *count)
{
    wchar_t pattern[4096],path[4096]; WIN32_FIND_DATAW found; HANDLE search;
    *count=0; swprintf_s(pattern,4096,L"%s\\ready-*.txt",directory);
    search=FindFirstFileW(pattern,&found); if(search==INVALID_HANDLE_VALUE) return 0;
    do {
        FILE *file=NULL; unsigned long pid; unsigned role; unsigned long long creation;
        swprintf_s(path,4096,L"%s\\%s",directory,found.cFileName); _wfopen_s(&file,path,L"rb");
        if(file) { if(fscanf_s(file,"pid=%lu role=%u creation=%llu",&pid,&role,&creation)==3 && *count<16) { fixtures[*count].pid=(uint32_t)pid; fixtures[*count].role=role; fixtures[(*count)++].creation=(uint64_t)creation; } fclose(file); }
    } while(FindNextFileW(search,&found));
    FindClose(search); return *count==4;
}
static int sample(ResourceCollector *collector,const Fixture *fixtures,size_t count,PgTracker *tracker,PgMembership *state,int exited)
{
    ResourceBatch batch={0}; MonitorRow rows[16]; MonitorSnapshot metrics={0}; PgSnapshot *relations=NULL; PgGroups *groups=NULL;
    uint32_t error=0; size_t i,j,n=0; int result=1;
    if(resource_collector_collect(collector,&batch,&error)!=RESOURCE_OK) return 0;
    memset(rows,0,sizeof(rows));
    for(i=0;i<count;++i) for(j=0;j<batch.count;++j) if(batch.items[j].pid==fixtures[i].pid && batch.items[j].creation_time_utc_100ns==fixtures[i].creation) {
        rows[n].raw=batch.items[j]; rows[n].name=(char *)batch.items[j].process_name_utf8;
        if(!rows[n].name) rows[n].name="fixture identity unavailable";
        ++n; break;
    }
    metrics.rows=rows; metrics.count=n; metrics.measured_utc_100ns=batch.finished_at_utc_100ns;
    if(n!=(exited?3u:4u) || !pg_collect_windows(tracker,&metrics,NULL,&relations) || !pg_groups_build(state,&metrics,relations,&groups)) result=0;
    if(result) {
        double direct[4]={0},grouped[4]={0}; size_t valid[4]={0},group_valid[4]={0};
        for(i=0;i<n;++i) {
            const ResourceData *r=&rows[i].raw;
            if((r->valid_fields&RESOURCE_VALID_CPU) && isfinite(r->cpu_percent) && r->cpu_percent>=0 && r->cpu_interval_seconds>0) { direct[0]+=r->cpu_percent; ++valid[0]; }
            if(r->valid_fields&RESOURCE_VALID_MEMORY) { direct[1]+=(double)r->working_set_bytes/1048576; ++valid[1]; }
            if((r->valid_fields&RESOURCE_VALID_IO_READ) && isfinite(r->io_read_bytes_per_second) && r->io_read_bytes_per_second>=0 && r->io_interval_seconds>0) { direct[2]+=r->io_read_bytes_per_second; ++valid[2]; }
            if((r->valid_fields&RESOURCE_VALID_IO_WRITE) && isfinite(r->io_write_bytes_per_second) && r->io_write_bytes_per_second>=0 && r->io_interval_seconds>0) { direct[3]+=r->io_write_bytes_per_second; ++valid[3]; }
            printf("fixture_row pid=%lu parent=%lu grade=%s owner=%zu cpu=%.6f ws=%llu read=%.3f write=%.3f\n",(unsigned long)r->pid,(unsigned long)relations->relations[i].raw_parent_pid,pg_grade_name(relations->relations[i].grade),groups->owner[i],r->cpu_percent,(unsigned long long)r->working_set_bytes,r->io_read_bytes_per_second,r->io_write_bytes_per_second);
        }
        for(i=0;i<groups->count;++i) for(j=0;j<4;++j) { grouped[j]+=groups->groups[i].metric[j].value; group_valid[j]+=groups->groups[i].metric[j].valid; }
        for(j=0;j<4;++j) if(valid[j]!=group_valid[j] || fabs(direct[j]-grouped[j])>1e-7*fmax(1,direct[j])) result=0;
        if(groups->count!=(exited?2u:1u)) result=0;
        printf("fixture_sample parent_exited=%d rows=%zu groups=%zu inferred=%zu historical=%zu sums_equal=%d\n",exited,n,groups->count,relations->inferred,relations->historical,result);
        if(exited && !relations->historical) result=0;
    }
    pg_groups_free(groups); pg_snapshot_free(relations); resource_batch_free(&batch); return result;
}
int wmain(int argc,wchar_t **argv)
{
    Fixture fixtures[16]; size_t count=0,i; ResourceCollector *collector=NULL; PgTracker *tracker=pg_tracker_create(); PgMembership *state=pg_membership_create();
    uint32_t error=0; int result=0; wchar_t marker[4096];
    if(argc<2 || argc>3 || !tracker || !state) return 2;
    for(i=0;i<100;++i) { if(read_markers(argv[1],fixtures,&count)) break; Sleep(50); }
    if(count!=4 || resource_collector_create(NULL,&collector,&error)!=RESOURCE_OK) goto cleanup;
    if(!sample(collector,fixtures,count,tracker,state,0)) goto cleanup;
    Sleep(250);
    if(!sample(collector,fixtures,count,tracker,state,0)) goto cleanup;
    if(argc==3 && !wcscmp(argv[2],L"--parent-exit")) {
        FILE *file=NULL; HANDLE root=NULL;
        for(i=0;i<count;++i) if(fixtures[i].role==0) root=OpenProcess(SYNCHRONIZE,FALSE,fixtures[i].pid);
        if(!root) goto cleanup;
        swprintf_s(marker,4096,L"%s\\exit-root",argv[1]); _wfopen_s(&file,marker,L"wb"); if(file) fclose(file);
        if(!file || WaitForSingleObject(root,5000)!=WAIT_OBJECT_0) { CloseHandle(root); goto cleanup; }
        CloseHandle(root); Sleep(100);
        if(!sample(collector,fixtures,count,tracker,state,1)) goto cleanup;
    }
    result=1;
cleanup:
    resource_collector_destroy(collector); pg_membership_destroy(state); pg_tracker_destroy(tracker);
    printf("fixture_probe result=%s\n",result?"PASS":"FAIL"); return result?0:1;
}
