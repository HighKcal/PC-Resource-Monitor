#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <io.h>
#include "monitor.h"
#include "../../third_party/sqlite/sqlite3.h"
#define T UINT64_C(10000000)
static unsigned checks,failures;
#define CHECK(x) do{++checks;if(!(x)){++failures;fprintf(stderr,"FAIL line %u: %s\n",(unsigned)__LINE__,#x);}}while(0)
void *review_calloc(size_t n,size_t size){return calloc(n,size);}
typedef struct ProbeResult { uint32_t valid; ResourceDeltaStatus cpu,read,write; uint64_t utc,end; unsigned ram_samples; double duration; } ProbeResult;
static int collect_probe(ResourceCollector *collector,MonitorEngine *engine,const char *tag,LARGE_INTEGER *last,LARGE_INTEGER freq,ProbeResult *result){
    ResourceBatch all={0},one;MonitorSnapshot *snapshot=NULL;uint32_t error=0;size_t i;LARGE_INTEGER end;double gap;
    CHECK(resource_collector_collect(collector,&all,&error)==RESOURCE_OK);QueryPerformanceCounter(&end);gap=last->QuadPart?(double)(end.QuadPart-last->QuadPart)/(double)freq.QuadPart:0;*last=end;
    for(i=0;i<all.count&&all.items[i].pid!=GetCurrentProcessId();i++){} CHECK(i<all.count);if(i==all.count){resource_batch_free(&all);return 0;}
    one=all;one.items=&all.items[i];one.count=1;CHECK(monitor_ingest(engine,&one,NULL,&snapshot));CHECK(snapshot!=NULL);
    if(snapshot){MonitorRow *r=&snapshot->rows[0];result->valid=r->raw.valid_fields;result->cpu=r->raw.cpu_status;result->read=r->raw.io_read_status;result->write=r->raw.io_write_status;result->utc=r->raw.measured_at_utc_100ns;result->end=r->metric[0].window_end_utc_100ns;result->ram_samples=r->metric[1].valid_samples;result->duration=r->metric[0].window_duration_seconds;
        printf("PROBE %s self_pid=%lu actual_utc=%llu collect_end_QPC_gap_s=%.6f obs_valid=%u cpu=%s read=%s write=%s last_window_end=%llu duration=%.6f RAM_valid_samples=%u completed=%zu\n",tag,(unsigned long)r->raw.pid,(unsigned long long)result->utc,gap,(result->valid&RESOURCE_VALID_OBSERVATION_INTERVAL)?1u:0u,resource_delta_status_string(result->cpu),resource_delta_status_string(result->read),resource_delta_status_string(result->write),(unsigned long long)result->end,result->duration,result->ram_samples,snapshot->completed_windows);fflush(stdout);}
    {int collected=snapshot!=NULL;monitor_snapshot_free(snapshot);resource_batch_free(&all);return collected;}
}
static void clear_owned(const char *file){char side[300];_unlink(file);snprintf(side,sizeof side,"%s-wal",file);_unlink(side);snprintf(side,sizeof side,"%s-shm",file);_unlink(side);}
int main(int argc,char **argv){
    const char *bench_file="build-review-monitor/mature-probe.sqlite",*probe_file="build-review-monitor/mature-probe-real.sqlite";MonitorEngine *bench=NULL,*probe=NULL;ResourceCollector *collector=NULL;sqlite3 *db=NULL;sqlite3_stmt *q=NULL;char error[256],wal[32768];char *paths[500]={0};const char *selected[500];ResourceData rows[500];ResourceBatch batch;MonitorSnapshot *snapshot=NULL;LARGE_INTEGER freq,last={0},begin,end;ProbeResult before={0},after={0},recovery={0};unsigned programs=0,i,j,row_offset_ms=1;uint64_t anchor=0;uint32_t os_error=0;double bench_ms;WIN32_FILE_ATTRIBUTE_DATA wal_info;
    if(argc<2||argc>3){fprintf(stderr,"test_mature_probe CHECKPOINTED_SYNTHETIC_FIXTURE_DB [ROW_OFFSET_MS default1]\n");return 2;}if(argc==3){row_offset_ms=(unsigned)strtoul(argv[2],NULL,10);if(row_offset_ms>1000)return 2;}
    snprintf(wal,sizeof wal,"%s-wal",argv[1]);if(GetFileAttributesExA(wal,GetFileExInfoStandard,&wal_info)&&(wal_info.nFileSizeHigh||wal_info.nFileSizeLow)){fprintf(stderr,"Source must be closed/checkpointed; nonempty WAL present\n");return 2;}
    clear_owned(bench_file);clear_owned(probe_file);CHECK(CopyFileA(argv[1],bench_file,FALSE));CHECK(monitor_open(bench_file,1,&bench,error,sizeof error));if(!bench){fprintf(stderr,"bench open: %s\n",error);return 1;}
    CHECK(sqlite3_open_v2(bench_file,&db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);CHECK(sqlite3_prepare_v2(db,"SELECT path FROM program ORDER BY id LIMIT 500",-1,&q,NULL)==SQLITE_OK);
    while(sqlite3_step(q)==SQLITE_ROW&&programs<500){paths[programs]=_strdup((const char*)sqlite3_column_text(q,0));CHECK(paths[programs]!=NULL);programs++;}sqlite3_finalize(q);q=NULL;CHECK(programs>0);if(!programs){monitor_close(bench);sqlite3_close(db);return 1;}
    CHECK(sqlite3_prepare_v2(db,"SELECT COALESCE(max(end_utc),0) FROM aggregate_window",-1,&q,NULL)==SQLITE_OK);CHECK(sqlite3_step(q)==SQLITE_ROW);anchor=(uint64_t)sqlite3_column_int64(q,0)+120*T;sqlite3_finalize(q);q=NULL;sqlite3_close(db);db=NULL;CHECK(anchor>120*T);
    printf("SYNTHETIC BENCH copied_fixture=%s target_instances=500 path_count=%u synthetic_anchor_utc=%llu row_offset_ms=%u; REAL PROBE uses separate DB and actual UTC only\n",argv[1],programs,(unsigned long long)anchor,row_offset_ms);fflush(stdout);
    memset(rows,0,sizeof rows);memset(&batch,0,sizeof batch);batch.items=rows;batch.count=500;batch.logical_processor_count=12;
    for(j=0;j<500;j++){selected[j]=paths[j%programs];rows[j].pid=40000+j;rows[j].creation_time_utc_100ns=anchor-3600*T+j;rows[j].process_name_utf8="SYNTHETIC mature probe.exe";rows[j].valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_NAME|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL;rows[j].cpu_percent=1;rows[j].working_set_bytes=67108864;rows[j].observation_interval_seconds=rows[j].cpu_interval_seconds=rows[j].io_interval_seconds=3;}
    for(i=0;i<20;i++){for(j=0;j<500;j++)rows[j].measured_at_utc_100ns=anchor+(uint64_t)i*3*T+(uint64_t)j*row_offset_ms*(T/1000);batch.started_at_utc_100ns=rows[0].measured_at_utc_100ns;batch.finished_at_utc_100ns=rows[499].measured_at_utc_100ns;CHECK(monitor_ingest(bench,&batch,selected,&snapshot));CHECK(snapshot!=NULL);if(snapshot)CHECK(snapshot->completed_windows==0);monitor_snapshot_free(snapshot);snapshot=NULL;}
    CHECK(monitor_open(probe_file,0,&probe,error,sizeof error));CHECK(resource_collector_create(NULL,&collector,&os_error)==RESOURCE_OK);CHECK(QueryPerformanceFrequency(&freq));if(!probe||!collector)return 1;
    printf("REAL PROBE warmup 21 rounds x nominal3s to obtain a completed actual window; no new real processes\n");fflush(stdout);
    for(i=0;i<=20;i++){char tag[40];if(i)Sleep(3000);snprintf(tag,sizeof tag,"warm-%u",i);if(!collect_probe(collector,probe,tag,&last,freq,&before))return 1;}
    CHECK(before.end>0);CHECK(before.ram_samples>=15);CHECK(before.cpu==RESOURCE_DELTA_VALID);
    for(j=0;j<500;j++)rows[j].measured_at_utc_100ns=anchor+60*T+(uint64_t)j*row_offset_ms*(T/1000);batch.started_at_utc_100ns=rows[0].measured_at_utc_100ns;batch.finished_at_utc_100ns=rows[499].measured_at_utc_100ns;
    QueryPerformanceCounter(&begin);CHECK(monitor_ingest(bench,&batch,selected,&snapshot));QueryPerformanceCounter(&end);bench_ms=(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart;CHECK(snapshot!=NULL);
    if(snapshot){unsigned ready=0;CHECK(snapshot->completed_windows==500);for(j=0;j<500;j++){unsigned m;for(m=0;m<4;m++){CHECK(snapshot->rows[j].metric[m].count>=60);CHECK(snapshot->rows[j].metric[m].days>=3);CHECK(snapshot->rows[j].metric[m].minutes>=60);if(snapshot->rows[j].metric[m].state>=MONITOR_NORMAL)ready++;}}printf("SYNTHETIC BENCH 500 simultaneous_close completed=%zu ready_metrics=%u outer_ingest_ms=%.3f store_ms=%.3f\n",snapshot->completed_windows,ready,bench_ms,snapshot->store_ms);fflush(stdout);}monitor_snapshot_free(snapshot);snapshot=NULL;
    Sleep(3000);if(!collect_probe(collector,probe,"after-bench-plus-worker3s",&last,freq,&after))return 1;
    if(after.cpu==RESOURCE_DELTA_LONG_GAP){CHECK(!(after.valid&RESOURCE_VALID_OBSERVATION_INTERVAL));CHECK(!(after.valid&RESOURCE_VALID_CPU));CHECK(after.end==0);printf("REAL PROBE long_gap observed; actual monitor completed-window evidence cleared/reset\n");}else{CHECK(after.cpu==RESOURCE_DELTA_VALID);CHECK(after.valid&RESOURCE_VALID_OBSERVATION_INTERVAL);CHECK(after.end>=before.end);printf("REAL PROBE no collector long_gap in this measured case\n");}
    Sleep(3000);if(!collect_probe(collector,probe,"recovery-plus3s",&last,freq,&recovery))return 1;CHECK(recovery.cpu==RESOURCE_DELTA_VALID);CHECK(recovery.valid&RESOURCE_VALID_OBSERVATION_INTERVAL);if(after.cpu==RESOURCE_DELTA_LONG_GAP)CHECK(recovery.end==0);
    resource_collector_destroy(collector);monitor_close(probe);monitor_close(bench);for(j=0;j<programs;j++)free(paths[j]);printf("MATURE PROBE %u checks, %u failures; synthetic performance and actual collector observations are separate\n",checks,failures);return failures?1:0;
}
