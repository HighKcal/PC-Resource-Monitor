/* monitor_gui.c에 포함하는 비공개 상세 표시 구현이다. 현재 원시값과 마지막 완료창 보고를
 * 구분해 읽으며 수치·판정·저장 상태를 바꾸지 않는다. */
#ifndef MONITOR_NATIVE_DETAIL_H
#define MONITOR_NATIVE_DETAIL_H
static const char *ui_metric(unsigned metric)
{
    static const char *names[]={"CPU","RAM · Working Set","읽기","쓰기"};
    return metric<MONITOR_METRICS?names[metric]:"알 수 없는 지표";
}
static const char *ui_unit(unsigned metric) { return metric==MONITOR_CPU?"%":metric==MONITOR_RAM?"GB":"MB/s"; }
/* CPU는 전체 논리 CPU 대비 %, RAM은 Working Set bytes/1e9, I/O는 B/s/1e6로 표시한다. */
static double ui_converted(double value,unsigned metric) { return value/(metric==MONITOR_CPU?1.0:metric==MONITOR_RAM?1e9:1e6); }
static const char *ui_value(char *buffer,size_t size,int valid,double value,unsigned metric)
{
    double converted=ui_converted(value,metric);
    if(!valid || !isfinite(converted)) snprintf(buffer,size,"NA");
    else if(converted>0 && converted<0.01) snprintf(buffer,size,"<0.01");
    else snprintf(buffer,size,"%.2f",converted);
    return buffer;
}
static const char *ui_delta(ResourceDeltaStatus state)
{
    switch(state) {
    case RESOURCE_DELTA_VALID: return "유효";
    case RESOURCE_DELTA_FIRST_SAMPLE: return "첫 표본 · 변화량 대기";
    case RESOURCE_DELTA_IDENTITY_UNAVAILABLE: return "프로세스 신원 확인 불가";
    case RESOURCE_DELTA_IDENTITY_CHANGED: return "프로세스 신원 변경";
    case RESOURCE_DELTA_QUERY_FAILED: return "조회 실패";
    case RESOURCE_DELTA_CLOCK_INVALID: return "관측 시간 무효";
    case RESOURCE_DELTA_LONG_GAP: return "긴 관측 공백";
    case RESOURCE_DELTA_COUNTER_ROLLBACK: return "누적 카운터 역행";
    default: return "확인 불가";
    }
}
static const char *ui_missing_delta(ResourceDeltaStatus state)
{
    return state==RESOURCE_DELTA_VALID?"측정값 확인 불가":ui_delta(state);
}
static const char *ui_interval(char *buffer,size_t size,int valid,double seconds,ResourceDeltaStatus state)
{
    if(valid && isfinite(seconds) && seconds>0) snprintf(buffer,size,"%.3f초",seconds);
    else snprintf(buffer,size,"%s",state==RESOURCE_DELTA_FIRST_SAMPLE?"측정 대기":"확인 불가");
    return buffer;
}
/* 기존 reason 원문의 부분 문자열을 사용자 설명으로 옮기는 표시 계층이다.
 * 판정을 재계산하지 않으며 원문이 바뀌면 이 대응 관계도 확인해야 한다. */
static const char *ui_reason(const MonitorMetricReport *report)
{
    if(strstr(report->reason,"identity")) return "프로세스 신원을 확인할 수 없어 학습과 비교를 진행하지 않습니다.";
    if(strstr(report->reason,"path")) return "검증 가능한 실행 경로가 없어 학습과 비교를 진행하지 않습니다.";
    if(strstr(report->reason,"latest metric missing") || strstr(report->reason,"previous values retained"))
        return "최근 표본의 해당 지표가 결측되어 비교 상태와 연속 횟수를 중단했습니다. 아래의 보존 수치는 이전 완료창 자료이며 현재 판정이 아닙니다.";
    if(strstr(report->reason,"coverage") || strstr(report->reason,"valid sample")) return "완료창의 유효 관측 범위 또는 유효 표본 수가 부족합니다.";
    if(!report->window_end_utc_100ns) return "이 인스턴스에 아직 완료된 관측창이 없습니다. 첫 표본이나 초기화 후에는 새 관측창을 모읍니다.";
    if(report->state==MONITOR_LEARNING) return "평소 기준을 만들 과거 자료가 더 필요합니다. 60개 창 · UTC 3날짜 · 서로 다른 60분을 모두 충족해야 합니다.";
    if(report->state==MONITOR_CANDIDATE) return "같은 지표가 3개 이상 연속 완료창에서 상한을 넘었습니다. 변화 후보이며 유해성이나 필요성의 판정은 아닙니다.";
    if(report->state==MONITOR_ATTENTION) return "이 완료창이 평소 상한을 넘었습니다. 연속 횟수를 함께 확인하세요.";
    if(report->state==MONITOR_NORMAL) return "이 지표의 완료창 값이 과거 기준의 상한 이내입니다. 다른 지표나 프로그램 전체의 안전 판정은 아닙니다.";
    return "이 지표는 현재 비교할 수 없습니다. 진단 원문을 확인하세요.";
}
static void ui_detail_text(const char *text,int kind)
{
    int first=(int)SendMessageW(app.detail,EM_GETFIRSTVISIBLELINE,0,0),preserve=0;
    PgIdentity key=persistent_identity(selected_row()); size_t group=group_at(selected_item());
    if(kind==1) preserve=app.detail_kind==kind && key.pid && pg_identity_equal(key,app.detail_key) && app.shown_detail_metric==app.detail_metric;
    else if(kind==2 && group!=PG_NONE) preserve=app.detail_kind==kind && pg_group_key_equal(app.groups->groups[group].key,app.detail_group);
    app.detail_kind=kind; app.detail_key=key; app.shown_detail_metric=app.detail_metric;
    if(kind==2 && group!=PG_NONE) app.detail_group=app.groups->groups[group].key;
    set_text(app.detail,text);
    if(preserve && first>0) SendMessageW(app.detail,EM_LINESCROLL,0,first);
}
static void show_group_detail(size_t index)
{
    const PgGroup *g=&app.groups->groups[index]; char text[12288],line[1024],value[128],first[64],last[64];
    unsigned metric; size_t used;
    snprintf(text,sizeof(text),"%s%s\r\n현재 구성원 %zu개 · %s\r\n\r\n"
        "현재 표본의 합계\r\n관측 시작 %s\r\n관측 끝 %s\r\n",
        display_group_name(index),display_group_name_note(index),g->count,owner_name(g->key.kind),utc_text(g->observed_min,first,sizeof(first)),utc_text(g->observed_max,last,sizeof(last)));
    for(metric=0;metric<MONITOR_METRICS;++metric) {
        snprintf(line,sizeof(line),"%s  %s %s\r\n  유효 %zu / 전체 %zu개%s%s\r\n",
            ui_metric(metric),sum_text(&g->metric[metric],value,sizeof(value),metric),ui_unit(metric),g->metric[metric].valid,g->metric[metric].total,
            !g->metric[metric].valid?" · 전체 측정 불가":g->metric[metric].valid<g->metric[metric].total?" · 부분합":"",g->metric[metric].overflow?" · 합계 초과 오류":"");
        used=strlen(text); snprintf(text+used,sizeof(text)-used,"%s",line);
        if(metric!=MONITOR_RAM && g->metric[metric].valid) {
            used=strlen(text); snprintf(text+used,sizeof(text)-used,"  관측 간격 %.3f ~ %.3f초\r\n",g->metric[metric].interval_min,g->metric[metric].interval_max);
        }
    }
    used=strlen(text); snprintf(text+used,sizeof(text)-used,
        "\r\n구성원별 기여 보기\r\n이름 앞 + 또는 오른쪽 화살표로 펼친 뒤 구성원을 선택하세요. 검색해도 합계는 원래 전체 구성원 범위를 유지합니다.\r\n\r\n"
        "평소와 비교하려면 개별 구성원을 선택하세요. 그룹 Baseline과 그룹 알림 억제는 제공하지 않습니다.\r\n\r\n"
        "합계의 의미\r\nRAM은 Working Set 합계로 공유 페이지가 중복될 수 있습니다. CPU·I/O는 서로 다른 프로세스 관측 구간의 합계입니다. NA는 결측이며 0이 아닙니다.\r\n\r\n"
        "그룹 소속\r\n실행 관계는 추정이며 앱 전체 소속이나 실제 생성 주체를 증명하지 않습니다. 목록 이름은 참고용이며 그룹 소속이나 신원을 정하는 데 쓰지 않습니다. 현재 %zu개 관계는 과거에 관측됐고, 표시 경계에서 %zu개 관계를 분리했습니다.\r\n\r\n"
        "아래의 구성원 확정은 현재 인스턴스들만 지정합니다. 새 프로세스는 자동 상속하지 않습니다. 지정·대상·펼침은 이번 세션에만 유지되고 재개/재시작하면 초기화됩니다.\r\n"
        "현재 이동 대상: %s\r\n\r\n%s",
        g->historical,g->scope_splits,pg_target_id(app.membership)?pg_target_label(app.membership):"지정 없음",
        app.ui_demo?"합성 화면 검수 · 수집과 저장을 실행하지 않습니다.":"자료는 이 PC에 로컬 저장됩니다.");
    ui_detail_text(text,2);
}
static void show_detail(void)
{
    int index=selected_row(); size_t group=group_at(selected_item()),used; unsigned m=(unsigned)app.detail_metric;
    MonitorRow *row; MonitorMetricReport *report; MonitorFeedback feedback; uint64_t feedback_expires;
    char text[16384],observed[64],created[64],expires[64],start[64],end[64],v[4][96],median[96],mad[96],threshold[96],current[96],cpu_interval[64],io_interval[64];
    const PgRelation *relation; PgSnapshot *relations; const ResourceDisplayInfo *info;
    update_actions();
    EnableWindow(GetDlgItem(app.window,ID_METRIC),index>=0);
    if(index<0 && group!=PG_NONE) { show_group_detail(group); return; }
    if(index<0 || !app.display || (size_t)index>=app.display->count) {
        snprintf(text,sizeof(text),"목록에서 프로그램이나 구성원을 선택하세요.\r\n\r\n"
            "현재 사용량 → 구성원별 기여 → 평소와 비교\r\n\r\n"
            "그룹을 펼치면 각 구성원의 현재 사용량을 볼 수 있습니다. 개별 프로세스를 선택하면 마지막 완료창과 과거 기준을 비교합니다.\r\n\r\n"
            "그룹 합계는 현재 표본입니다. *는 일부 구성원만 유효한 부분합이며 선택하면 유효 X / 전체 Y를 표시합니다. 검색은 원래 합계 범위를 바꾸지 않습니다.\r\n\r\n"
            "CPU는 전체 논리 CPU 대비 %%입니다. RAM은 Working Set의 decimal GB, 읽기/쓰기는 decimal MB/s입니다. 공유 페이지가 RAM 합계에 중복될 수 있습니다. NA는 결측이며 0과 다릅니다.\r\n\r\n"
            "그룹 지정은 이번 세션에만 유지됩니다. 재개하면 새 관측창을 모으고 소속·대상·선택·펼침을 초기화합니다.\r\n\r\n"
            "원시 표본 1시간 / 요약 28일 보관. 높은 사용량·신규 프로그램·비전경만으로 유해하거나 불필요하다고 판단하지 않습니다.\r\n\r\n"
            "진단 원문\r\n%s",app.display?app.display->status:"아직 표본 없음");
        ui_detail_text(text,0); return;
    }
    row=&app.display->rows[index]; if(m>=MONITOR_METRICS) m=MONITOR_CPU; report=&row->metric[m];
    info=display_info((size_t)index);
    feedback=displayed_feedback(row,&feedback_expires);
    ui_value(v[0],sizeof(v[0]),row->raw.valid_fields&RESOURCE_VALID_CPU,row->raw.cpu_percent,MONITOR_CPU);
    ui_value(v[1],sizeof(v[1]),row->raw.valid_fields&RESOURCE_VALID_MEMORY,(double)row->raw.working_set_bytes,MONITOR_RAM);
    ui_value(v[2],sizeof(v[2]),row->raw.valid_fields&RESOURCE_VALID_IO_READ,row->raw.io_read_bytes_per_second,MONITOR_READ);
    ui_value(v[3],sizeof(v[3]),row->raw.valid_fields&RESOURCE_VALID_IO_WRITE,row->raw.io_write_bytes_per_second,MONITOR_WRITE);
    snprintf(text,sizeof(text),"%s\r\nPID %lu · %s\r\n표시 이름: %s\r\n\r\n"
        "현재 사용량 · 최근 표본\r\n%s\r\nCPU %s %%    RAM %s GB\r\n읽기 %s MB/s    쓰기 %s MB/s\r\n"
        "CPU 간격 %s · I/O 간격 %s\r\n\r\n"
        "%s · 개별 완료창 비교\r\n%s: %s\r\n",
        display_name((size_t)index),(unsigned long)row->raw.pid,row->raw.foreground==RESOURCE_FOREGROUND?"순간 전경":row->raw.foreground==RESOURCE_BACKGROUND?"순간 비전경":"전경 확인 불가",
        ui_name_source_text(ui_name_source(row,info)),
        utc_text(row->raw.measured_at_utc_100ns,observed,sizeof(observed)),v[0],v[1],v[2],v[3],
        ui_interval(cpu_interval,sizeof(cpu_interval),row->raw.valid_fields&RESOURCE_VALID_CPU,row->raw.cpu_interval_seconds,row->raw.cpu_status),
        ui_interval(io_interval,sizeof(io_interval),row->raw.valid_fields&(RESOURCE_VALID_IO_READ|RESOURCE_VALID_IO_WRITE),row->raw.io_interval_seconds,row->raw.io_read_status),
        ui_metric(m),app.shown && app.shown->session!=app.session?"이전 세션 보고 상태 · 새 표본 대기":"현재 비교 상태",ui_state(report->state));
    if(!(row->raw.valid_fields&RESOURCE_VALID_NAME)) {
        const char *name_reason=ui_field_query_reason(row,info,row->raw.name_error);
        used=strlen(text); snprintf(text+used,sizeof(text)-used,"핸들 이름 조회: %s\r\n",
            name_reason?name_reason:"이름을 확인할 수 없습니다. 목록 이름이 있으면 참고용으로만 표시합니다.");
    }
    /* 현재 수치 가까이에 지표별 결측 원인을 붙여 이전 완료창 값과 혼동하지 않게 한다. */
    {
        char missing[2048]=""; size_t n=0; const char *reason;
        if(!(row->raw.valid_fields&RESOURCE_VALID_CPU)) { reason=ui_field_query_reason(row,info,row->raw.times_error); n=strlen(missing); snprintf(missing+n,sizeof(missing)-n,"CPU: %s\r\n",reason?reason:ui_missing_delta(row->raw.cpu_status)); }
        if(!(row->raw.valid_fields&RESOURCE_VALID_MEMORY)) { reason=ui_field_query_reason(row,info,row->raw.memory_error); n=strlen(missing); snprintf(missing+n,sizeof(missing)-n,"RAM: %s\r\n",reason?reason:"측정값 확인 불가"); }
        if(!(row->raw.valid_fields&RESOURCE_VALID_IO_READ)) { reason=ui_field_query_reason(row,info,row->raw.io_error); n=strlen(missing); snprintf(missing+n,sizeof(missing)-n,"읽기: %s\r\n",reason?reason:ui_missing_delta(row->raw.io_read_status)); }
        if(!(row->raw.valid_fields&RESOURCE_VALID_IO_WRITE)) { reason=ui_field_query_reason(row,info,row->raw.io_error); n=strlen(missing); snprintf(missing+n,sizeof(missing)-n,"쓰기: %s\r\n",reason?reason:ui_missing_delta(row->raw.io_write_status)); }
        if(missing[0]) {
            char *anchor=strstr(text,"CPU 간격");
            if(anchor) { char tail[2048]; snprintf(tail,sizeof(tail),"%s",anchor); *anchor=0; used=strlen(text); snprintf(text+used,sizeof(text)-used,"%s%s",missing,tail); }
        }
    }
    if(report->window_end_utc_100ns) {
        used=strlen(text); snprintf(text+used,sizeof(text)-used,
            "완료창 값  %s %s\r\n과거 중앙값  %s %s\r\n%s  %s %s\r\n%s\r\n\r\n"
            "%s\r\n\r\n완료창 시작 %s\r\n완료창 끝 %s\r\n실제 길이 %.3f초\r\n과거 MAD  %s %s\r\n\r\n"
            "과거 자료 %u개 창 / UTC %u날짜 / %u분\r\n학습 기준 60개 창 / 3날짜 / 60분\r\n"
            "유효 관측 %.3f초 · 표본 %u개\r\n연속 상한 초과 %u창\r\n이 창의 학습 포함: %s\r\n\r\n",
            ui_value(current,sizeof(current),isfinite(report->current),report->current,m),ui_unit(m),
            ui_value(median,sizeof(median),isfinite(report->median),report->median,m),ui_unit(m),
            report->state==MONITOR_LEARNING?"학습 중 상한 계산값":report->state==MONITOR_UNAVAILABLE?"참고 상한 계산값":"평소 상한",ui_value(threshold,sizeof(threshold),isfinite(report->threshold),report->threshold,m),ui_unit(m),
            report->state==MONITOR_LEARNING?"학습 중 계산값 · 비교 기준 미확정":report->state==MONITOR_UNAVAILABLE?"보존/참고 수치 · 현재 비교 기준이 아닙니다.":"완료창 수치이며 현재 표본과 별개입니다.",ui_reason(report),
            utc_text(report->window_start_utc_100ns,start,sizeof(start)),utc_text(report->window_end_utc_100ns,end,sizeof(end)),report->window_duration_seconds,
            ui_value(mad,sizeof(mad),isfinite(report->mad),report->mad,m),ui_unit(m),
            report->count,report->days,report->minutes,report->covered_seconds,report->valid_samples,report->streak,
            report->baseline_eligible?"포함 대상":"제외 / 평가 불가");
    } else {
        used=strlen(text); snprintf(text+used,sizeof(text)-used,"%s\r\n\r\n완료창 없음 · 비교 수치가 아직 없습니다.\r\n\r\n",
            !row->program_id || !row->path?"프로세스 신원 또는 정확한 실행 경로를 확인할 수 없어 학습과 비교를 진행하지 않습니다.":ui_reason(report));
    }
    used=strlen(text); snprintf(text+used,sizeof(text)-used,
        "이 경로의 알림 억제\r\n%s\r\n%s\r\n측정·학습·후보 근거는 유지되며 정확히 같은 경로에만 적용됩니다.\r\n%s\r\n\r\n"
        "평소 기준의 의미\r\n현재 창보다 과거인 자료만 사용합니다. 해당 UTC 날짜와 이전 13날짜에서 날짜당 최근 64개 분대표를 고릅니다.\r\n"
        "상한 = 중앙값 + max(3 × 1.4826 × MAD, 중앙값의 50%%, 최소폭). 최소폭은 CPU 5%%p, RAM 0.134217728 GB, I/O 1.048576 MB/s입니다. RAM은 유효 표본 15개 이상, CPU/I/O는 창 길이 대비 유효 관측 75~125%%가 필요합니다.\r\n\r\n"
        "대상 확인\r\n생성 %s\r\n인스턴스 신원: %s\r\n정확한 경로: %s\r\n경로 확인: %s\r\n현재 소속: %s%s / %s\r\n현재 이동 대상: %s\r\n"
        "소속 지정은 이번 세션에만 유지됩니다. 실행 관계는 추정이며 완전한 앱 소속의 증거가 아닙니다.\r\n\r\n",
        feedback==MONITOR_FEEDBACK_IGNORE?"1시간 억제 · 로컬 저장 확인됨":feedback==MONITOR_FEEDBACK_EXPECTED?"24시간 예상 사용 · 로컬 저장 확인됨":"억제 없음 / 만료",
        feedback==MONITOR_FEEDBACK_CANCEL?"기한 해당 없음 · 억제 없음 / 만료":utc_text(feedback_expires,expires,sizeof(expires)),app.ui_demo?"합성 화면 검수에서는 알림 억제를 저장하지 않습니다.":"버튼을 누른 뒤 worker 저장 확인이 도착하면 적용 상태를 표시합니다.",
        utc_text(row->raw.valid_fields&RESOURCE_VALID_IDENTITY?row->raw.creation_time_utc_100ns:0,created,sizeof(created)),ui_identity_reason(row,info),row->path?row->path:"확인 불가 · 학습 제외",ui_path_reason(row),
        group!=PG_NONE?display_group_name(group):"개별 표시",group!=PG_NONE?display_group_name_note(group):"",group!=PG_NONE?owner_name(app.groups->groups[group].key.kind):"관계 정보 없음",
        pg_target_id(app.membership)?pg_target_label(app.membership):"지정 없음");
    if(info && info->special_process!=RESOURCE_SPECIAL_NONE) {
        used=strlen(text); snprintf(text+used,sizeof(text)-used,"운영체제 특수 항목: %s\r\n일반 프로세스와 조회 조건이 다릅니다. 접근 거부만으로 보호 프로세스 여부를 판단하지 않습니다.\r\n\r\n",
            info->special_process==RESOURCE_SPECIAL_IDLE?"System Idle Process (PID 0)":"System (PID 4)");
    }
    relations=current_relations(); relation=relations?&relations->relations[index]:NULL;
    used=strlen(text); snprintf(text+used,sizeof(text)-used,
        "진단 상세 · 원문\r\n상태: %s\r\nreport: %s; %s\r\n"
        "CPU: %s\r\n읽기: %s\r\n쓰기: %s\r\n"
        "CPU/read/write 원문: %s / %s / %s\r\n"
        "valid_fields=0x%lx; WS=%llu bytes\r\n"
        "Win32 open/times/memory/io/name/path=%lu/%lu/%lu/%lu/%lu/%lu\r\n"
        "display_source=%u; metadata=%u; attempted=0x%lx; list_name_error=%lu\r\n"
        "exit_observed=%d; exit_code=%lu; exit_query_error=%lu\r\n"
        "open 실패 시 후속 API는 실행되지 않으며 0 오류 코드는 성공을 뜻하지 않습니다. path는 신원 미확인 시 실행하지 않습니다.\r\n"
        "관계 원문: %s / %s; raw PPID=%lu; error=%lu\r\n"
        "수집 %.3fms · 저장 %.3fms · 관계 %.3fms\r\n%s",
        app.display->status,monitor_state_name(report->state),report->reason,ui_delta(row->raw.cpu_status),ui_delta(row->raw.io_read_status),ui_delta(row->raw.io_write_status),
        resource_delta_status_string(row->raw.cpu_status),resource_delta_status_string(row->raw.io_read_status),resource_delta_status_string(row->raw.io_write_status),
        (unsigned long)row->raw.valid_fields,(unsigned long long)row->raw.working_set_bytes,
        (unsigned long)row->raw.open_error,(unsigned long)row->raw.times_error,(unsigned long)row->raw.memory_error,(unsigned long)row->raw.io_error,(unsigned long)row->raw.name_error,(unsigned long)row->path_error,
        (unsigned)ui_name_source(row,info),info?1u:0u,(unsigned long)(info?info->attempted_fields:0),(unsigned long)(info?info->snapshot_name_error:0),
        info?info->exit_observed:0,(unsigned long)(info?info->exit_code:0),(unsigned long)(info?info->exit_query_error:0),
        relation?pg_grade_name(relation->grade):"unavailable",relation?pg_cut_name(relation->cut):"flat fallback",(unsigned long)(relation?relation->raw_parent_pid:0),(unsigned long)(relation?relation->error:0),
        app.display->collect_ms,app.display->store_ms,app.shown?app.shown->grouping_ms:0,app.ui_demo?"합성값이며 엔진/수집 결과가 아닙니다.":"원시 표본 1시간 / 요약 28일 보관");
    ui_detail_text(text,1);
}
#endif
