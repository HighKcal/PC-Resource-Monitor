/* monitor_gui.c의 비공개 합성 화면 자료다. 고정된 예시로 표시 경로를 확인하며
 * DB·수집기·engine을 실행하지 않는다. 실제 판정이나 장기 학습 결과가 아니다. */
#ifndef MONITOR_NATIVE_DEMO_H
#define MONITOR_NATIVE_DEMO_H
static GuiSnapshot *ui_demo_snapshot(void)
{
    GuiSnapshot *view=calloc(1,sizeof(*view)); MonitorSnapshot *snapshot;
    PgProbe probes[44]; uint64_t now=UINT64_C(134383644000000000); size_t i; unsigned m;
    if(!view) return NULL;
    snapshot=calloc(1,sizeof(*snapshot)); view->metrics=snapshot;
    if(!snapshot) { free(view); return NULL; }
    snapshot->rows=calloc(44,sizeof(*snapshot->rows));
    if(!snapshot->rows) { gui_snapshot_free(view); return NULL; }
    snapshot->count=44; snapshot->synthetic=1; snapshot->measured_utc_100ns=now; snapshot->sequence=1;
    snapshot->foreground_pid=1000; snapshot->foreground_observed_utc_100ns=now;
    snprintf(snapshot->status,sizeof(snapshot->status),"UI DEMO: fixed synthetic values; engine and collector not executed");
    memset(probes,0,sizeof(probes));
    for(i=0;i<snapshot->count;++i) {
        MonitorRow *r=&snapshot->rows[i]; char name[256],path[512];
        const char *names[]={"ChatGPT.exe","ChatGPT helper.exe","ChatGPT renderer.exe","ChatGPT GPU.exe","ChatGPT utility.exe","ChatGPT audio.exe",
            "한글 문서 도구.exe","문서 도우미.exe","파일 미리보기.exe","한글 검색 색인.exe","문서 동기화.exe","글꼴 서비스.exe",
            "VeryLongEnglishApplicationNameForColumnResizeAndEllipsisVerification.exe","개인 일정.exe","Code.exe","Terminal.exe","음악 플레이어.exe","작은 읽기 0.00001 MB.exe"};
        if(i==40) snprintf(name,sizeof(name),"VeryLongEnglishApplicationNameForColumnResizeAndEllipsisVerification.exe");
        else if(i==41) snprintf(name,sizeof(name),"합성 큰수치 스트레스 예시.exe");
        else if(i==42) snprintf(name,sizeof(name),"신원 확인 불가 예시.exe");
        else if(i==43) snprintf(name,sizeof(name),"작은 양수 읽기 0.00001 MB.exe");
        else if(i<sizeof(names)/sizeof(names[0])) snprintf(name,sizeof(name),"%s",names[i]);
        else snprintf(name,sizeof(name),"%s %02zu.exe",i%2?"합성 프로그램":"Synthetic application",i);
        snprintf(path,sizeof(path),"SYNTHETIC:\\Program %02zu\\%s",i,name);
        r->name=_strdup(name); r->path=_strdup(path); r->program_id=(int64_t)i+1;
        if(!r->name || !r->path) { gui_snapshot_free(view); return NULL; }
        r->raw.process_name_utf8=r->name; r->raw.pid=(uint32_t)i+1000;
        r->raw.creation_time_utc_100ns=now-UINT64_C(6000000000)+(uint64_t)i*10000000;
        r->raw.measured_at_utc_100ns=now; r->raw.cpu_percent=i<6?1.4+(double)i*0.6:(double)(i%7)*0.21;
        r->raw.working_set_bytes=i<6?UINT64_C(460000000)+(uint64_t)i*70000000:UINT64_C(12500000)+(uint64_t)i*11000000;
        r->raw.io_read_bytes_per_second=i==43?10.0:(double)(i%6)*120000.0;
        r->raw.io_write_bytes_per_second=(double)(i%4)*50000.0;
        if(i==41) { r->raw.working_set_bytes=UINT64_C(1234560000000); r->raw.io_read_bytes_per_second=12345670000.0; }
        r->raw.valid_fields=RESOURCE_VALID_NAME|RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_MEMORY|
            RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_FOREGROUND|RESOURCE_VALID_OBSERVATION_INTERVAL;
        r->raw.foreground=i==0?RESOURCE_FOREGROUND:RESOURCE_BACKGROUND;
        r->raw.cpu_interval_seconds=r->raw.io_interval_seconds=r->raw.observation_interval_seconds=3.01;
        r->raw.cpu_status=r->raw.io_read_status=r->raw.io_write_status=RESOURCE_DELTA_VALID;
        probes[i].grade=PG_PID_ONLY;
        if((i>0 && i<6) || (i>6 && i<40)) {
            size_t root=i<6?0:6;
            probes[i].grade=PG_CURRENT_INFERRED; probes[i].parent_pid_valid=1;
            probes[i].parent_pid=(uint32_t)root+1000;
            probes[i].parent_creation=now-UINT64_C(6000000000)+(uint64_t)root*10000000;
        }
        for(m=0;m<MONITOR_METRICS;++m) {
            MonitorMetricReport *report=&r->metric[m];
            report->state=i==6?MONITOR_UNAVAILABLE:i%5==0?MONITOR_LEARNING:MONITOR_NORMAL;
            report->current=i==6?NAN:m==MONITOR_CPU?1.0:m==MONITOR_RAM?250000000.0:100000.0;
            report->median=i==6?NAN:m==MONITOR_CPU?1.0:m==MONITOR_RAM?200000000.0:80000.0;
            report->mad=i==6?NAN:m==MONITOR_RAM?8000000.0:0.1;
            report->threshold=i==6?NAN:m==MONITOR_CPU?6.0:m==MONITOR_RAM?334217728.0:1128576.0;
            report->count=i==6?0:i%5==0?24:128; report->days=i==6?0:i%5==0?2:7; report->minutes=i==6?0:i%5==0?24:128;
            report->covered_seconds=i==6?0:60.2; report->valid_samples=i==6?0:20; report->window_duration_seconds=i==6?0:60.2;
            report->window_start_utc_100ns=i==6?0:now-UINT64_C(700000000);
            report->window_end_utc_100ns=i==6?0:now-UINT64_C(98000000);
            report->baseline_eligible=i!=6;
            snprintf(report->reason,sizeof(report->reason),"%s",i==6?"no completed instance window":i%5==0?"history gate not met":"within threshold");
        }
        if(i==0) { r->metric[0].state=MONITOR_CANDIDATE; r->metric[0].current=14.1; r->metric[0].streak=3; r->metric[0].count=128; r->metric[0].days=7; r->metric[0].minutes=128; r->metric[0].baseline_eligible=0; snprintf(r->metric[0].reason,sizeof(r->metric[0].reason),"three consecutive windows above threshold"); }
        if(i==1) r->raw.valid_fields&=~RESOURCE_VALID_IO_WRITE;
        if(i==3) { r->raw.valid_fields&=~RESOURCE_VALID_CPU; r->raw.cpu_status=RESOURCE_DELTA_QUERY_FAILED; r->metric[0].state=MONITOR_UNAVAILABLE; r->metric[0].streak=0; snprintf(r->metric[0].reason,sizeof(r->metric[0].reason),"current sample unavailable; previous values retained"); }
        if(i==6) { r->raw.valid_fields&=~(RESOURCE_VALID_CPU|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE); r->raw.cpu_status=r->raw.io_read_status=r->raw.io_write_status=RESOURCE_DELTA_FIRST_SAMPLE; }
        if(i==42) {
            r->raw.valid_fields&=~(RESOURCE_VALID_IDENTITY|RESOURCE_VALID_CPU|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE);
            r->raw.creation_time_utc_100ns=0; r->program_id=0; free(r->path); r->path=NULL;
            r->raw.cpu_status=r->raw.io_read_status=r->raw.io_write_status=RESOURCE_DELTA_IDENTITY_UNAVAILABLE;
            for(m=0;m<MONITOR_METRICS;++m) { memset(&r->metric[m],0,sizeof(r->metric[m])); r->metric[m].current=r->metric[m].median=r->metric[m].mad=r->metric[m].threshold=NAN; snprintf(r->metric[m].reason,sizeof(r->metric[m].reason),"identity unavailable"); }
        }
    }
    view->session=app.session;
    if(!pg_resolve(NULL,snapshot,probes,&view->relations)) { gui_snapshot_free(view); return NULL; }
    return view;
}
static void ui_demo_first_sample(GuiSnapshot *view)
{
    size_t i; unsigned m;
    if(!view || !view->metrics) return;
    for(i=0;i<view->metrics->count;++i) {
        MonitorRow *row=&view->metrics->rows[i];
        row->raw.valid_fields&=~(RESOURCE_VALID_CPU|RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE|RESOURCE_VALID_OBSERVATION_INTERVAL);
        row->raw.cpu_interval_seconds=row->raw.io_interval_seconds=row->raw.observation_interval_seconds=0;
        row->raw.cpu_status=row->raw.io_read_status=row->raw.io_write_status=row->raw.valid_fields&RESOURCE_VALID_IDENTITY?RESOURCE_DELTA_FIRST_SAMPLE:RESOURCE_DELTA_IDENTITY_UNAVAILABLE;
        for(m=0;m<MONITOR_METRICS;++m) {
            MonitorMetricReport *report=&row->metric[m]; memset(report,0,sizeof(*report));
            report->state=row->program_id?MONITOR_LEARNING:MONITOR_UNAVAILABLE;
            report->current=report->median=report->mad=report->threshold=NAN;
            snprintf(report->reason,sizeof(report->reason),"collecting complete 60-75s window");
        }
    }
    snprintf(view->metrics->status,sizeof(view->metrics->status),"UI DEMO: resumed first-sample display; no collector or engine executed");
}
#endif
