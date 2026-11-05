#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "program_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Frozen deterministic input, no engine/DB writes. Hash the original raw rows
 * before/after resolver+ownership+sum projection. Timings are pure model cost. */
static uint64_t digest(const MonitorRow *rows,size_t count)
{
    const unsigned char *p; uint64_t h=UINT64_C(1469598103934665603); size_t i,j;
    for(i=0;i<count;++i) {
        ResourceData raw=rows[i].raw; raw.process_name_utf8=NULL;
        p=(const unsigned char *)&raw;
        for(j=0;j<sizeof(raw);++j) h=(h^p[j])*UINT64_C(1099511628211);
        p=(const unsigned char *)rows[i].metric;
        for(j=0;j<sizeof(rows[i].metric);++j) h=(h^p[j])*UINT64_C(1099511628211);
    }
    return h;
}
int main(int argc,char **argv)
{
    size_t n=500,rounds=100,i,round; MonitorRow *rows; PgProbe *probes;
    MonitorSnapshot metrics={0}; PgTracker *tracker=pg_tracker_create(); PgMembership *state=pg_membership_create();
    uint64_t before,after; LARGE_INTEGER begin,end,freq; double total=0,maximum=0;
    if(argc>1) n=(size_t)strtoul(argv[1],NULL,10); if(argc>2) rounds=(size_t)strtoul(argv[2],NULL,10);
    if(n<1 || n>100000 || rounds<1 || rounds>10000) return 2;
    rows=calloc(n,sizeof(*rows)); probes=calloc(n,sizeof(*probes)); if(!rows || !probes || !tracker || !state) return 1;
    for(i=0;i<n;++i) {
        size_t root=(i/150)*150;
        rows[i].name="deterministic.exe"; rows[i].path="SYNTHETIC:\\same.exe"; rows[i].raw.pid=(uint32_t)i+1; rows[i].raw.creation_time_utc_100ns=(uint64_t)i+1000;
        rows[i].raw.valid_fields=RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE;
        rows[i].raw.cpu_percent=(double)(i%13)/10; rows[i].raw.working_set_bytes=(i+1)*4096; rows[i].raw.cpu_interval_seconds=2.9+(double)(i%7)/100;
        rows[i].raw.io_interval_seconds=3.0+(double)(i%11)/100; rows[i].raw.io_read_bytes_per_second=(double)i; rows[i].raw.io_write_bytes_per_second=(double)i/2;
        if(i!=root) { probes[i].grade=PG_CURRENT_INFERRED; probes[i].parent_pid_valid=1; probes[i].parent_pid=(uint32_t)root+1; probes[i].parent_creation=(uint64_t)root+1000; }
    }
    metrics.rows=rows; metrics.count=n; metrics.measured_utc_100ns=100000; before=digest(rows,n);
    QueryPerformanceFrequency(&freq);
    for(round=0;round<rounds;++round) {
        PgSnapshot *relations=NULL; PgGroups *groups=NULL; size_t counted=0;
        QueryPerformanceCounter(&begin);
        if(!pg_resolve(tracker,&metrics,probes,&relations) || !pg_groups_build(state,&metrics,relations,&groups)) return 1;
        QueryPerformanceCounter(&end);
        { double elapsed=(double)(end.QuadPart-begin.QuadPart)*1000/(double)freq.QuadPart; total+=elapsed; if(elapsed>maximum) maximum=elapsed; }
        for(i=0;i<groups->count;++i) counted+=groups->groups[i].count;
        if(counted!=n) return 1;
        pg_groups_free(groups); pg_snapshot_free(relations);
    }
    after=digest(rows,n);
    printf("replay rows=%zu rounds=%zu off_raw_digest=%016llx on_raw_digest=%016llx equal=%d model_mean_ms=%.6f model_max_ms=%.6f\n",n,rounds,(unsigned long long)before,(unsigned long long)after,before==after,total/(double)rounds,maximum);
    free(rows); free(probes); pg_membership_destroy(state); pg_tracker_destroy(tracker); return before==after?0:1;
}
