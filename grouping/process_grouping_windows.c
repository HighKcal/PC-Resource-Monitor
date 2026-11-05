/* 첫 Toolhelp 부모 PID → 현재 핸들 생성시각 → 두 번째 부모 PID → 양쪽 생존 순서로
 * 대조한다. OS 부모의 현재 인스턴스를 추정할 뿐 과거 생성 사건을 재현하지 않는다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>
#include "process_grouping.h"

typedef struct ProbeRow {
    uint32_t pid, first_parent, second_parent;
    size_t row;
    int unique, first_found, second_found, identity;
    HANDLE handle;
    uint32_t error;
} ProbeRow;
static int compare(const void *a,const void *b)
{
    const ProbeRow *x=a,*y=b;
    return x->pid==y->pid?0:x->pid<y->pid?-1:1;
}
static ProbeRow *find(ProbeRow *rows,size_t n,uint32_t pid)
{
    ProbeRow key; memset(&key,0,sizeof(key)); key.pid=pid;
    return bsearch(&key,rows,n,sizeof(*rows),compare);
}
static int stopped(HANDLE stop) { return stop && WaitForSingleObject(stop,0)==WAIT_OBJECT_0; }
static uint32_t enumerate(ProbeRow *rows,size_t n,int second,HANDLE stop)
{
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    DWORD error=0;
    snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE) return GetLastError();
    memset(&entry,0,sizeof(entry)); entry.dwSize=sizeof(entry);
    if(!Process32FirstW(snapshot,&entry)) error=GetLastError();
    else do {
        ProbeRow *row;
        if(stopped(stop)) { error=ERROR_CANCELLED; break; }
        row=find(rows,n,entry.th32ProcessID);
        if(row && row->unique) {
            if(second) { row->second_found=1; row->second_parent=entry.th32ParentProcessID; }
            else { row->first_found=1; row->first_parent=entry.th32ParentProcessID; }
        }
        if(!Process32NextW(snapshot,&entry)) { DWORD last=GetLastError(); if(last!=ERROR_NO_MORE_FILES) error=last; break; }
    } while(1);
    CloseHandle(snapshot); return error;
}
int pg_collect_windows(PgTracker *tracker,const MonitorSnapshot *metrics,void *stop_event,PgSnapshot **out)
{
    ProbeRow *rows=NULL; PgProbe *probes=NULL;
    size_t i,n; uint32_t error=0; int result=0;
    HANDLE stop=(HANDLE)stop_event;
    if(!out) return 0;
    *out=NULL;
    if(!metrics || metrics->count>SIZE_MAX/sizeof(*rows) || metrics->count>SIZE_MAX/sizeof(*probes)) return 0;
    n=metrics->count;
    if(!n) return pg_resolve(tracker,metrics,NULL,out);
    rows=calloc(n,sizeof(*rows)); probes=calloc(n,sizeof(*probes));
    if(!rows || !probes) goto cleanup;
    for(i=0;i<n;++i) { rows[i].pid=metrics->rows[i].raw.pid; rows[i].row=i; rows[i].unique=1; probes[i].grade=PG_PROBE_FAILED; }
    qsort(rows,n,sizeof(*rows),compare);
    for(i=0;i<n;) {
        size_t end=i+1,j;
        while(end<n && rows[end].pid==rows[i].pid) ++end;
        if(end-i>1) for(j=i;j<end;++j) rows[j].unique=0;
        i=end;
    }
    error=stopped(stop)?ERROR_CANCELLED:enumerate(rows,n,0,stop);
    for(i=0;i<n;++i) {
        ProbeRow *p=&rows[i]; PgProbe *probe=&probes[p->row];
        PgIdentity key=pg_row_identity(&metrics->rows[p->row]); FILETIME created,exited,kernel,user;
        probe->parent_pid=p->first_parent; probe->parent_pid_valid=p->first_found;
        if(!key.pid) { probe->grade=PG_IDENTITY_UNAVAILABLE; continue; }
        if(error || !p->unique || !p->first_found || stopped(stop)) { probe->error=error?error:ERROR_INVALID_DATA; continue; }
        p->handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,p->pid);
        if(!p->handle) { p->error=GetLastError(); continue; }
        if(!GetProcessTimes(p->handle,&created,&exited,&kernel,&user)) { p->error=GetLastError(); continue; }
        if((((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime)!=key.creation) { p->error=ERROR_INVALID_DATA; continue; }
        p->identity=1;
    }
    if(!error) error=stopped(stop)?ERROR_CANCELLED:enumerate(rows,n,1,stop);
    /* 두 번째 목록과 양쪽 생존 확인이 모두 끝날 때까지 모든 프로세스 핸들을 유지한다.
     * 나중에 같은 PID를 다시 여는 방식으로는 이 인스턴스 대조를 대신할 수 없다. */
    for(i=0;i<n;++i) {
        ProbeRow *p=&rows[i],*parent=find(rows,n,p->first_parent);
        PgProbe *probe=&probes[p->row];
        if(probe->grade==PG_IDENTITY_UNAVAILABLE) continue;
        probe->error=error?error:p->error;
        if(error || !p->unique || !p->identity || !p->first_found || !p->second_found || p->first_parent!=p->second_parent) {
            if(!probe->error) probe->error=ERROR_INVALID_DATA;
            continue;
        }
        probe->grade=PG_PID_ONLY;
        if(!parent || !parent->unique || !parent->identity) { if(parent) probe->error=parent->error; continue; }
        if(WaitForSingleObject(p->handle,0)!=WAIT_TIMEOUT || WaitForSingleObject(parent->handle,0)!=WAIT_TIMEOUT) { probe->error=ERROR_PROCESS_ABORTED; continue; }
        probe->parent_creation=pg_row_identity(&metrics->rows[parent->row]).creation;
        probe->grade=PG_CURRENT_INFERRED;
    }
    result=pg_resolve(tracker,metrics,probes,out);
    if(result) (*out)->probe_error=error;
cleanup:
    if(rows) for(i=0;i<n;++i) if(rows[i].handle) CloseHandle(rows[i].handle);
    free(rows); free(probes); return result;
}
