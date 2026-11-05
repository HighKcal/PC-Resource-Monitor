/* monitor_gui.c의 비공개 검색·정렬 구현이다. 원본 행과 소속·합계를 수정하지 않고
 * 표시 인덱스를 비교한다. 개별 행 동률은 PID·생성시각·원본 위치로, 그룹 동률은 그룹 인덱스로 구분한다. */
#ifndef MONITOR_NATIVE_PROJECTION_H
#define MONITOR_NATIVE_PROJECTION_H
static int ui_matches_text(const char *text)
{
    wchar_t *wide,*cursor; size_t length=wcslen(app.search); int found=0;
    if(!length) return 1;
    wide=to_wide(text?text:""); if(!wide) return 0;
    for(cursor=wide;*cursor;++cursor) if(!_wcsnicmp(cursor,app.search,length)) { found=1; break; }
    free(wide); return found;
}
static int ui_matches_row(size_t source)
{
    const MonitorRow *row=&app.display->rows[source]; char pid[32];
    if(!app.search[0]) return 1;
    snprintf(pid,sizeof(pid),"%lu",(unsigned long)row->raw.pid);
    return ui_matches_text(display_name(source)) || ui_matches_text(row->path) || ui_matches_text(pid);
}
static int ui_group_matches(size_t group)
{
    const PgGroup *g=&app.groups->groups[group]; size_t j;
    if(ui_matches_text(display_group_name(group))) return 1;
    for(j=0;j<g->count;++j) if(ui_matches_row(app.groups->members[g->first+j])) return 1;
    return 0;
}
static int ui_numeric_compare(int av,double a,int bv,double b)
{
    int ascending=app.sort_mode%2==0;
    if(av!=bv) return av?-1:1; /* 오름·내림차순 모두 결측은 유효한 0을 포함한 측정값 뒤에 둔다. */
    if(!av || a==b) return 0;
    return (a<b?-1:1)*(ascending?1:-1);
}
static int __cdecl ui_source_compare(void *unused,const void *a,const void *b)
{
    size_t ai=*(const size_t *)a,bi=*(const size_t *)b; const MonitorRow *ar=&app.display->rows[ai],*br=&app.display->rows[bi]; int result=0;
    (void)unused;
    if(app.sort_mode>=3) {
        unsigned metric=(unsigned)(app.sort_mode-3)/2;
        unsigned bit=metric==MONITOR_CPU?RESOURCE_VALID_CPU:metric==MONITOR_RAM?RESOURCE_VALID_MEMORY:metric==MONITOR_READ?RESOURCE_VALID_IO_READ:RESOURCE_VALID_IO_WRITE;
        double av=metric==MONITOR_CPU?ar->raw.cpu_percent:metric==MONITOR_RAM?(double)ar->raw.working_set_bytes:metric==MONITOR_READ?ar->raw.io_read_bytes_per_second:ar->raw.io_write_bytes_per_second;
        double bv=metric==MONITOR_CPU?br->raw.cpu_percent:metric==MONITOR_RAM?(double)br->raw.working_set_bytes:metric==MONITOR_READ?br->raw.io_read_bytes_per_second:br->raw.io_write_bytes_per_second;
        result=ui_numeric_compare((ar->raw.valid_fields&bit)!=0 && isfinite(av),av,(br->raw.valid_fields&bit)!=0 && isfinite(bv),bv);
    } else if(app.sort_mode) result=_stricmp(display_name(ai),display_name(bi))*(app.sort_mode==2?-1:1);
    if(result) return result;
    if(ar->raw.pid!=br->raw.pid) return ar->raw.pid<br->raw.pid?-1:1;
    if(ar->raw.creation_time_utc_100ns!=br->raw.creation_time_utc_100ns) return ar->raw.creation_time_utc_100ns<br->raw.creation_time_utc_100ns?-1:1;
    return ai==bi?0:ai<bi?-1:1;
}
static int __cdecl ui_group_compare(void *unused,const void *a,const void *b)
{
    size_t ai=*(const size_t *)a,bi=*(const size_t *)b; const PgGroup *ar=&app.groups->groups[ai],*br=&app.groups->groups[bi]; int result=0;
    (void)unused;
    if(app.sort_mode>=3) {
        unsigned metric=(unsigned)(app.sort_mode-3)/2; const PgSum *as=&ar->metric[metric],*bs=&br->metric[metric];
        double av=metric==MONITOR_RAM?(double)as->bytes:as->value,bv=metric==MONITOR_RAM?(double)bs->bytes:bs->value;
        result=ui_numeric_compare(as->valid!=0 && !as->overflow,av,bs->valid!=0 && !bs->overflow,bv);
    } else if(app.sort_mode) result=_stricmp(display_group_name(ai),display_group_name(bi))*(app.sort_mode==2?-1:1);
    if(result) return result;
    return ai==bi?0:ai<bi?-1:1;
}
#endif
