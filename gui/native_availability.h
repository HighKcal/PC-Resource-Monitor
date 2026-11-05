/* monitor_gui.c에 포함하는 비공개 표시 구현 조각이다. 같은 회차의 이름·조회 진단만 읽으며
 * engine의 신원·경로·유효성·그룹 키·과거 자료 입력으로 사용하지 않는다. */
#ifndef MONITOR_NATIVE_AVAILABILITY_H
#define MONITOR_NATIVE_AVAILABILITY_H
#include "resource_display.h"

static const ResourceDisplayInfo *ui_display_metadata(const ResourceDisplayBatch *display,
    const MonitorSnapshot *snapshot,size_t source)
{
    const ResourceDisplayInfo *info; const ResourceData *raw;
    if(!display || !snapshot || !display->items || !snapshot->rows ||
        display->count!=snapshot->count || source>=snapshot->count) return NULL;
    /* 이름 sidecar를 다른 회차나 PID 재사용 행에 붙이지 않도록 개수·위치·tuple·관측시각을 대조한다. */
    info=&display->items[source]; raw=&snapshot->rows[source].raw;
    if(info->pid!=raw->pid || info->measured_at_utc_100ns!=raw->measured_at_utc_100ns ||
        info->creation_time_utc_100ns!=((raw->valid_fields&RESOURCE_VALID_IDENTITY)?raw->creation_time_utc_100ns:0)) return NULL;
    return info;
}
static ResourceNameSource ui_name_source(const MonitorRow *row,const ResourceDisplayInfo *info)
{
    if((row->raw.valid_fields&RESOURCE_VALID_NAME) && row->raw.process_name_utf8 && row->raw.process_name_utf8[0]) return RESOURCE_NAME_HANDLE;
    if(info && info->name_source==RESOURCE_NAME_LIST && info->snapshot_name_utf8 && info->snapshot_name_utf8[0]) return RESOURCE_NAME_LIST;
    return RESOURCE_NAME_UNAVAILABLE;
}
static const char *ui_name_text(const MonitorRow *row,const ResourceDisplayInfo *info)
{
    ResourceNameSource source=ui_name_source(row,info);
    return source==RESOURCE_NAME_HANDLE?row->raw.process_name_utf8:
        source==RESOURCE_NAME_LIST?info->snapshot_name_utf8:"이름 확인 불가";
}
static const char *ui_name_source_text(ResourceNameSource source)
{
    return source==RESOURCE_NAME_HANDLE?"프로세스 핸들에서 조회한 이름":source==RESOURCE_NAME_LIST?
        "목록에서 관측한 참고 이름 · 이 이름만으로 인스턴스 신원이나 전체 경로를 확인할 수 없습니다.":
        "표시 이름을 얻지 못했습니다. 현재 목록에서 발견한 PID를 표시합니다.";
}
/* OpenProcess 실패는 후속 조회의 전제 실패다. 이를 개별 API 네 번의 실패로 세지 않는다.
 * ERROR_INVALID_PARAMETER만으로 프로세스 종료를 확정하지 않는다. */
static const char *ui_field_query_reason(const MonitorRow *row,const ResourceDisplayInfo *info,uint32_t field_error)
{
    if(row->raw.open_error==ERROR_ACCESS_DENIED) return "접근 권한 부족 · 프로세스를 열지 못해 후속 조회를 실행하지 못했습니다.";
    if(row->raw.open_error) return "프로세스를 열지 못했습니다. 종료 여부는 확인되지 않았습니다.";
    if(field_error && info && info->exit_observed) return "이 항목의 조회가 실패했고 이후 프로세스 종료가 확인되었습니다.";
    if(field_error==ERROR_ACCESS_DENIED) return "이 항목을 조회할 접근 권한이 부족합니다.";
    if(field_error) return "이 항목을 조회하지 못했습니다. 진단 상세에서 오류 코드를 확인할 수 있습니다.";
    return NULL;
}
static const char *ui_identity_reason(const MonitorRow *row,const ResourceDisplayInfo *info)
{
    const char *reason;
    if(row->raw.valid_fields&RESOURCE_VALID_IDENTITY) return "생성시각 확인됨 · PID와 생성시각으로 인스턴스를 연결합니다.";
    reason=ui_field_query_reason(row,info,row->raw.times_error);
    return reason?reason:"생성시각과 인스턴스 신원을 확인할 수 없습니다.";
}
static const char *ui_path_reason(const MonitorRow *row)
{
    if(row->path) return "생성시각을 대조해 확보한 정확한 실행 경로";
    if(!(row->raw.valid_fields&RESOURCE_VALID_IDENTITY)) return "신원이 확인되지 않아 경로 조회를 실행하지 않았습니다.";
    if(row->path_error==ERROR_ACCESS_DENIED) return "실행 경로를 조회할 접근 권한이 부족합니다.";
    if(row->path_error==ERROR_INVALID_DATA) return "경로 확인 중 생성시각이 달라 같은 인스턴스로 연결하지 않았습니다.";
    return "실행 경로 조회가 실패하여 학습과 비교에서 제외합니다.";
}
#endif
