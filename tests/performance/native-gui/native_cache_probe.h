/* Copied-build diagnostics only. All database access stays on its owning worker.
 * These SQLite categories are approximate current allocations, not process totals.
 * Status calls reset nothing, prepare no statements, and read no process data. */
#ifndef NATIVE_CACHE_PROBE_H
#define NATIVE_CACHE_PROBE_H
#include "monitor_fixture.h"
static uint64_t cache_probe_started;
static LARGE_INTEGER cache_probe_frequency;
static void cache_probe_emit(MonitorEngine *engine,const MonitorSnapshot *snapshot,unsigned batch,const char *phase)
{
    int cache=-1,statements=-1,schema=-1,highwater=0;
    int cache_rc,statement_rc,schema_rc,memory_valid;
    DWORD memory_error=0;
    PROCESS_MEMORY_COUNTERS_EX memory;
    LARGE_INTEGER observer_begin,observer_end;
    QueryPerformanceCounter(&observer_begin);
    if(!cache_probe_started) {
        cache_probe_started=GetTickCount64();
        QueryPerformanceFrequency(&cache_probe_frequency);
        printf("CACHE_PROBE start sqlite=%s source_id=%s pointer_bytes=%zu opaque_engine=1 reset=0 note=approx_current_category_bytes_not_process_total;sampled_on_owner_worker\n",
            sqlite3_libversion(),sqlite3_sourceid(),sizeof(void *));
    }
    cache_rc=sqlite3_db_status(monitor_fixture_db(engine),SQLITE_DBSTATUS_CACHE_USED,&cache,&highwater,0);
    statement_rc=sqlite3_db_status(monitor_fixture_db(engine),SQLITE_DBSTATUS_STMT_USED,&statements,&highwater,0);
    schema_rc=sqlite3_db_status(monitor_fixture_db(engine),SQLITE_DBSTATUS_SCHEMA_USED,&schema,&highwater,0);
    memset(&memory,0,sizeof(memory)); memory.cb=sizeof(memory);
    memory_valid=GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&memory,sizeof(memory))!=FALSE;
    if(!memory_valid) memory_error=GetLastError();
    printf("CACHE_PROBE sample phase=%s batch=%u uptime_ms=%llu rows=%zu windows=%zu instances=%zu sequence=%llu cache_rc=%d cache_bytes=%d stmt_rc=%d stmt_bytes=%d schema_rc=%d schema_bytes=%d memory_valid=%d memory_error=%lu private_bytes=%lld working_set_bytes=%lld\n",
        phase,batch,(unsigned long long)(GetTickCount64()-cache_probe_started),snapshot?snapshot->count:0,
        snapshot?snapshot->completed_windows:0,monitor_fixture_instance_count(engine),(unsigned long long)(snapshot?snapshot->sequence:0),
        cache_rc,cache_rc==SQLITE_OK?cache:-1,statement_rc,statement_rc==SQLITE_OK?statements:-1,schema_rc,schema_rc==SQLITE_OK?schema:-1,
        memory_valid,(unsigned long)memory_error,memory_valid?(long long)memory.PrivateUsage:-1,memory_valid?(long long)memory.WorkingSetSize:-1);
    fflush(stdout);
    QueryPerformanceCounter(&observer_end);
    printf("CACHE_PROBE observer batch=%u wall_ms=%.6f note=includes_status_memory_and_sample_log_flush;excludes_this_observer_line\n",
        batch,(double)(observer_end.QuadPart-observer_begin.QuadPart)*1000/(double)cache_probe_frequency.QuadPart);
    fflush(stdout);
}
#endif
