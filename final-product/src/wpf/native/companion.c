/* 기존 로컬 근거·공개 자료 provider·Shell을 WPF DLL에 연결한다. 프로그램 정보는
 * pc_program_collect → preview → execute, tray는 pc_tray_request/message에서 읽는다.
 * managed는 표시와 명시적 목적을 전달하고 기존 신원·전송·재진입 경계는 여기서 유지한다. */
#include "../../gui/program_research.h"
#include "../../gui/program_info.h"
#include "../../gui/official_role_summary.h"
#include "../../gui/native_program_info_browser.h"
#include "../../notifications/notification_shell.h"
#include "companion.h"
#include <stdarg.h>

typedef struct PcProgram {
    ProgramInfoResult local;
    ProgramResearchRequest preview;
    ProgramResearchResult research;
    ProgramResearchTranslation translation;
    unsigned intent,translation_index;
    int preview_kind,has_preview,has_research,has_translation;
    uint64_t serial;
    char translation_original[512],notice[1024],text[65536];
} PcProgram;

static void append_text(PcProgram *p,const char *format,...)
{
    size_t used=strlen(p->text);
    va_list args;
    if(used>=sizeof(p->text)-1) return;
    va_start(args,format);
    (void)vsnprintf(p->text+used,sizeof(p->text)-used,format,args);
    va_end(args);
}
static int public_field(char *out,size_t cap,const char *value)
{
    size_t n=value?strlen(value):0;
    if(!n || n>=cap) { if(cap) out[0]=0; return 0; }
    for(size_t i=0;i<n;++i)
        if((unsigned char)value[i]<32 || strchr("\\/:@%?#<>`$",value[i])) { out[0]=0; return 0; }
    memcpy(out,value,n+1); return 1;
}
/* 현재 로컬 신원·서명·파일 근거에서 공개 조회에 허용할 항목만 복사한다.
 * 화면용 메타데이터와 전송용 근거는 구분하며 실제 요청 가능 여부는 provider plan에 맡긴다. */
static int make_payload(PcProgram *p,unsigned intent,ProgramResearchRequest *request)
{
    ProgramInfoResult *local=&p->local;
    ProgramResearchPlan plan;
    memset(request,0,sizeof(*request));
    if(!program_info_result_current(local) || local->request.synthetic ||
        !local->signature_verified || local->signature_code!=ERROR_SUCCESS ||
        !local->file_stamp.hash_valid || local->homepage_conflict) return 0;
    if(!public_field(request->publisher,sizeof(request->publisher),local->publisher)) return 0;
    if(local->metadata_source_verified) {
        (void)public_field(request->product,sizeof(request->product),local->product);
        (void)public_field(request->description,sizeof(request->description),local->description);
        (void)public_field(request->company,sizeof(request->company),local->company);
        (void)public_field(request->original,sizeof(request->original),local->original);
    }
    if(!request->product[0]) (void)public_field(request->product,sizeof(request->product),local->opus_name);
    if(!request->product[0]) return 0;
    if(local->homepage_source && program_research_origin(local->homepage_origin,request->homepage_origin,sizeof(request->homepage_origin)))
        request->homepage_source=(ProgramResearchHomepageSource)local->homepage_source;
    request->session=local->request.session; request->intent=intent; request->local_identity_verified=1;
    return program_research_plan(request,&plan);
}
static int binding(PcProgram *p,HANDLE cancel,uint64_t deadline)
{
    ProgramInfoRequest request=p->local.request;
    request.cancel_event=cancel; request.deadline_tick=deadline;
    return program_info_result_current(&p->local) && program_info_recheck(&request,&p->local.file_stamp);
}
static int catalog_url(PcProgram *p,const char *url)
{
    const ProgramInfoRoleRecord *record=program_info_role_record(p->local.role);
    char origin[256];
    if(!program_research_origin(url,origin,sizeof(origin))) return 0;
    if(record) for(unsigned i=0;i<record->source_count && i<4;++i)
        if(!strcmp(url,record->sources[i].url)) return 1;
    return !strcmp(url,"https://support.microsoft.com/en-us/windows/experience/startup-boot/configure-startup-applications-in-windows");
}
/* ProgramInfoWindow의 작업에서 한 대상의 로컬 근거를 수집한다. 반환 PcProgram은
 * pc_program_destroy로 해제하며 취소 이벤트는 빌린 뒤 결과에서 제거한다. NULL은 할당 실패다. */
void *pc_program_collect(uint32_t pid,uint64_t creation,unsigned session,int synthetic,const char *path,HANDLE cancel)
{
    PcProgram *p=calloc(1,sizeof(*p));
    ProgramInfoRequest *request;
    if(!p) return NULL;
    request=&p->local.request;
    request->pid=pid; request->creation=creation; request->session=session; request->synthetic=synthetic;
    request->request_id=1; request->cancel_event=cancel; request->deadline_tick=GetTickCount64()+15000;
    if(path && strlen(path)<sizeof(request->path)) memcpy(request->path,path,strlen(path)+1);
    /* collect가 결과를 비우므로 그 안에 들어 있던 요청은 별도 값으로 먼저 복사한다. */
    { ProgramInfoRequest copied=*request; program_info_collect(&copied,&p->local); }
    p->local.request.cancel_event=NULL; p->local.request.deadline_tick=0;
    return p;
}
void pc_program_destroy(void *context)
{
    PcProgram *p=context;
    if(!p) return;
    program_info_release(&p->local); free(p);
}
int pc_program_current(void *context)
{
    PcProgram *p=context;
    return p && program_info_result_current(&p->local);
}
void pc_program_clear_preview(void *context)
{
    PcProgram *p=context;
    if(p) { p->has_preview=0; memset(&p->preview,0,sizeof(p->preview)); p->translation_original[0]=0; }
}
unsigned pc_program_source_count(void *context)
{
    PcProgram *p=context;
    return p?p->local.source_count+(p->has_research && p->research.url[0]?1u:0u):0;
}
const char *pc_program_source(void *context,unsigned index,int url)
{
    PcProgram *p=context;
    if(!p) return "";
    if(index<p->local.source_count && index<4) return url?p->local.sources[index].url:p->local.sources[index].title;
    if(index==p->local.source_count && p->has_research) return url?p->research.url:"조회한 공식 문서";
    return "";
}
const char *pc_program_text(void *context,unsigned intent,int details)
{
    PcProgram *p=context;
    char purpose[2048];
    if(!p) return "프로그램 정보 자원을 준비하지 못했습니다.";
    p->text[0]=0;
    program_info_purpose_text(&p->local,(ProgramInfoIntent)intent,purpose,sizeof(purpose));
    append_text(p,"일반 역할\r\n%s\r\n\r\n내 PC에서 확인한 내용\r\n%s\r\n%s\r\n\r\n내 선택과 영향\r\n%s\r\n\r\n다음 확인\r\n%s\r\n",
        p->local.role_text,p->local.identity_text,p->local.startup_text,purpose,p->local.next_text);
    if(!program_info_result_current(&p->local)) append_text(p,"\r\n현재 연결 미확인 · 종료·만료·실패를 정상이나 안전으로 해석하지 않습니다. 현재 목록에서 다시 확인하세요.\r\n");
    if(p->local.role==PROGRAM_INFO_ROLE_NONE) {
        const ProgramInfoDisplayMetadata *display=&p->local.display_metadata;
        append_text(p,"\r\n로컬 파일 단서\r\n제품: %s\r\n설명: %s\r\n회사 표기: %s\r\n서명 게시자: %s\r\n원래 파일명: %s\r\n표기 출처: %s\r\n",
            p->local.product[0]?p->local.product:display->product[0]?display->product:"확인 불가",
            p->local.description[0]?p->local.description:display->description[0]?display->description:"확인 불가",
            p->local.company[0]?p->local.company:display->company[0]?display->company:"확인 불가",
            p->local.publisher[0]?p->local.publisher:"확인 불가",p->local.original[0]?p->local.original:"확인 불가",
            p->local.metadata_source_verified?"파일 자체 리소스 · 현재 역할·작업의 증명은 아님":"MUI 병합 가능·출처 미확인 표시 문자열 · 역할에 연결하지 않음");
    }
    if(p->has_research) {
        OfficialRoleSummary summary;
        official_role_summarize(&p->research,&summary);
        append_text(p,"\r\n공식 자료 조사 결과\r\n%s\r\n%s\r\n공식 문서: %s\r\n",
            program_research_validate_opening_url(&p->research,p->research.url)?summary.text_ko:"선택한 제품과 연결되는 현재 공식 근거를 확정하지 못했습니다.",
            p->research.authority_text,p->research.url);
        if(summary.interpreted) append_text(p,"요약에 사용한 바로 그 문장:\r\n%s\r\n",summary.quote);
        for(unsigned i=0;i<p->research.evidence_count && i<3;++i) append_text(p,"\r\n공개 원문 %u:\r\n%s\r\n",i+1,p->research.evidence[i]);
        if(p->research.failure_text[0]) append_text(p,"확인 한계: %s\r\n",p->research.failure_text);
        append_text(p,"현재 이 인스턴스의 작업·실행 원인·안전·필요성을 확인한 결과는 아닙니다. 시작 설정·목적별 권고의 로컬 고정 자료와는 별개입니다.\r\n");
    }
    if(p->has_translation) append_text(p,"\r\n공개 원문 참고 번역\r\n%s\r\n%s\r\n원문: %s\r\n",
        p->translation.status==PROGRAM_RESEARCH_COMPLETE?p->translation.translated_ko:"참고 번역을 확인하지 못했습니다. 공개 원문을 확인하세요.",p->translation.notice,p->translation.original);
    if(details) {
        FILETIME ft={(DWORD)p->local.checked_utc,(DWORD)(p->local.checked_utc>>32)};
        SYSTEMTIME utc={0};
        (void)FileTimeToSystemTime(&ft,&utc);
        append_text(p,"\r\n자세한 근거\r\n%s\r\n검증 UTC %04u-%02u-%02u %02u:%02u:%02u\r\nPID %lu · 생성시각 %llu · 세션 %u\r\n실행 경로: %s\r\n",
            p->local.limits_text,(unsigned)utc.wYear,(unsigned)utc.wMonth,(unsigned)utc.wDay,
            (unsigned)utc.wHour,(unsigned)utc.wMinute,(unsigned)utc.wSecond,(unsigned long)p->local.request.pid,
            (unsigned long long)p->local.request.creation,p->local.request.session,p->local.request.path);
        for(unsigned i=0;i<p->local.source_count && i<4;++i)
            append_text(p,"\r\n출처: %s\r\n%s\r\n적용 범위: %s\r\n",p->local.sources[i].title,p->local.sources[i].url,p->local.sources[i].applies);
    }
    if(p->notice[0]) append_text(p,"\r\n%s\r\n",p->notice);
    return p->text;
}
const char *pc_program_preview(void *context,unsigned intent,int op)
{
    PcProgram *p=context;
    ProgramResearchPlan plan;
    if(!p) return "프로그램 정보를 먼저 확인하세요.";
    pc_program_clear_preview(p); p->text[0]=0;
    if(!program_info_result_current(&p->local)) return "현재 인스턴스 확인이 만료되었습니다. 새로고침하세요.";
    p->intent=intent; p->preview_kind=op;
    if(op==2) {
        if(p->has_research) for(unsigned i=0;i<p->research.evidence_count && i<3;++i)
            if(program_research_translation_preview(&p->research,i,p->translation_original,sizeof(p->translation_original))) {
                p->translation_index=i; p->has_preview=1;
                append_text(p,"공개 원문 번역 · 보내기 전 확인\r\n대상: api.mymemory.translated.net (MyMemory)\r\n보낼 공개 원문 한 문장:\r\n%s\r\n\r\n무료 참고 번역입니다. 문장이 장기 보관되거나 번역 파트너가 처리할 수 있습니다. ‘이 문장 번역’을 눌러 동의한 때만 위 문장 한 개를 전송합니다. 개인 기록이나 로컬 파일 정보는 보내지 않습니다.\r\n",p->translation_original);
                return p->text;
            }
        return "번역 가능한 현재 공식 원문 한 문장을 확인하지 못했습니다. 공개 원문과 공식 문서를 직접 확인하세요.";
    }
    if(op!=0 || !make_payload(p,intent,&p->preview) || !program_research_plan(&p->preview,&plan))
        return "공개 제품·서명 게시자·같은 파일의 내용 근거를 확인하지 못했습니다. 전송을 시작하지 않았습니다. 새로고침하거나 로컬 출처를 확인하세요.";
    p->has_preview=1;
    append_text(p,"공식 자료 조사 · 보내기 전 확인\r\n%s\r\n첫 홈페이지 GET: %s\r\nWikidata 검색에 보낼 정확한 제품명: %s\r\n정확한 검색 URL: %s\r\n\r\n로컬 대조에만 쓰는 단서 (검색에 보내지 않음)\r\n파일 설명: %s\r\n회사 표기: %s\r\n서명 게시자: %s\r\n원본 파일명: %s\r\n사용 목적: %s (검색문에 넣지 않음)\r\n\r\n‘이 내용으로 조회’를 누를 때만 전송합니다. 선택·목적을 바꾸거나 새로고침하면 이 동의는 끝납니다. 로컬 경로·PID·계정·자원 이력은 보내지 않습니다.\r\n",
        plan.disclosure_ko,plan.homepage_origin[0]?plan.homepage_origin:"확인된 주소 없음 · Wikidata부터 확인",p->preview.product,plan.wikidata_search_url,
        p->preview.description,p->preview.company,p->preview.publisher,p->preview.original,
        intent==1?"로그인 뒤 자동 준비":intent==2?"쓸 때 직접 실행":"미선택");
    return p->text;
}
int pc_program_preview_ready(void *context) { PcProgram *p=context; return p && p->has_preview; }
/* 모든 작업은 진입 시 현재 신원·파일을 다시 맞춘다. 조회(op=0)와 번역(op=2)은
 * 미리보기를 검증하고 완료 후에도 연결을 재확인한다. URL 열기(op=1)는 허용된 주소인지
 * 확인한 뒤 browser_open의 요청 결과를 돌려주며 사후 연결 확인은 하지 않는다.
 * 조회 결과는 임시 할당에서 완성한 뒤 p에 복사한다. 연결/미리보기 불일치는 동의를
 * 비우고 notice에 남기며, 표시할 실패 근거와 반환 성공 여부를 서로 구분한다. */
int pc_program_execute(void *context,unsigned intent,int op,unsigned source,HANDLE cancel)
{
    PcProgram *p=context;
    ProgramResearchRequest request;
    uint64_t deadline=GetTickCount64()+25000;
    if(!p) return 0;
    p->notice[0]=0;
    if(!binding(p,cancel,deadline)) goto changed;
    if(op==0) {
        ProgramResearchResult *result;
        if(!p->has_preview || p->preview_kind!=0 || p->intent!=intent ||
            !make_payload(p,intent,&request) || memcmp(&request,&p->preview,sizeof(request))) goto preview_changed;
        result=calloc(1,sizeof(*result));
        if(!result) return 0;
        request.request_id=++p->serial; request.cancel_event=cancel; request.deadline_tick=deadline; request.user_approved_public=1;
        p->has_preview=0;
        program_research_lookup(&request,result);
        if(!binding(p,cancel,deadline)) { free(result); goto changed; }
        result->request.cancel_event=NULL;
        if(!program_research_validate_opening_url(result,result->url)) {
            result->official_confirmed=result->product_matched=0;
            if(!result->failure_text[0]) snprintf(result->failure_text,sizeof(result->failure_text),"공식 문서·제품 연결·현재 유효한 인용 근거를 함께 확인하지 못했습니다.");
        }
        p->research=*result; free(result); p->has_research=1; p->has_translation=0;
        snprintf(p->notice,sizeof(p->notice),"%s",p->research.failure_text[0]?p->research.failure_text:p->research.official_confirmed?"공식 자료의 근거를 확인했습니다.":"공식 자료 연결을 확인하지 못했습니다.");
        return 1;
    }
    if(op==2) {
        char original[512];
        if(!p->has_preview || p->preview_kind!=2 || p->intent!=intent || !p->has_research ||
            !program_research_translation_preview(&p->research,p->translation_index,original,sizeof(original)) ||
            strcmp(original,p->translation_original)) goto preview_changed;
        p->has_preview=0; p->research.request.cancel_event=cancel;
        program_research_translate(&p->research,p->translation_index,1,&p->translation);
        p->research.request.cancel_event=NULL;
        if(!binding(p,cancel,deadline)) { memset(&p->translation,0,sizeof(p->translation)); goto changed; }
        p->has_translation=1;
        snprintf(p->notice,sizeof(p->notice),"%s",p->translation.status==PROGRAM_RESEARCH_COMPLETE?"공개 원문 한 문장의 참고 번역을 확인했습니다.":"참고 번역을 확인하지 못했습니다. 원문을 확인하세요.");
        return 1;
    }
    if(op==1) {
        const char *url=pc_program_source(p,source,1);
        int trusted=catalog_url(p,url) || (p->has_research && program_research_validate_opening_url(&p->research,url));
        HRESULT hr=trusted?program_info_browser_open(url):E_ACCESSDENIED;
        snprintf(p->notice,sizeof(p->notice),"%s",SUCCEEDED(hr)?"일반 권한의 기본 브라우저에 공식 문서 열기를 요청했습니다.":"일반 권한 브라우저에 문서를 열 수 없습니다. 주소를 복사해 직접 확인하세요.");
        return SUCCEEDED(hr);
    }
    return 0;
changed:
    /* 파일·인스턴스 재검증이 실패한 결과를 Current로 다시 허용하지 않는다.
     * managed 완료 지점도 이 무효화를 읽어 본문 수락과 다음 명령을 함께 막는다. */
    p->local.expires_tick=0;
    pc_program_clear_preview(p);
    snprintf(p->notice,sizeof(p->notice),"대상이 종료·만료되었거나 파일이 바뀌어 작업 또는 결과 연결을 중단했습니다. 현재 목록에서 다시 확인하세요.");
    return 0;
preview_changed:
    pc_program_clear_preview(p);
    snprintf(p->notice,sizeof(p->notice),"전송할 공개 항목·목적 또는 원문이 바뀌었습니다. 다시 미리보기를 확인하세요. 외부 요청을 보내지 않았습니다.");
    return 0;
}
typedef struct PcTray {
    NotificationShell shell;
    unsigned calls;
    int destroying;
} PcTray;
static void tray_leave(PcTray *tray)
{
    --tray->calls;
    if(tray->destroying && !tray->calls) free(tray);
}
void *pc_tray_create(HWND owner,UINT callback_message)
{
    PcTray *tray=calloc(1,sizeof(*tray));
    if(tray) notification_shell_init(&tray->shell,owner,callback_message);
    return tray;
}
int pc_tray_ready(void *context) { PcTray *tray=context; return tray && !tray->destroying && notification_shell_ready(&tray->shell); }
int pc_tray_request(void *context,const NotificationEvent *event,uint64_t now_ms)
{
    PcTray *tray=context;
    int result;
    if(!tray || tray->destroying || !event) return NOTIFICATION_SHELL_UNAVAILABLE;
    ++tray->calls;
    result=(int)notification_shell_request(&tray->shell,event,now_ms);
    tray_leave(tray); return result;
}
int pc_tray_message(void *context,UINT message,WPARAM wp,LPARAM lp,int enabled,int *action,uint64_t *id)
{
    PcTray *tray=context;
    NotificationShellAction result={0};
    int handled=0;
    if(tray && !tray->destroying) {
        ++tray->calls;
        handled=notification_shell_handle_message(&tray->shell,message,wp,lp,enabled!=0,&result);
        tray_leave(tray);
    }
    if(action) *action=(int)result.kind;
    if(id) *id=result.event_id;
    return handled;
}
uint64_t pc_tray_tick(void *context,uint64_t now_ms)
{
    PcTray *tray=context; uint64_t id;
    if(!tray || tray->destroying) return 0;
    ++tray->calls; id=notification_shell_tick(&tray->shell,now_ms); tray_leave(tray); return id;
}
void pc_tray_cancel(void *context,uint64_t id)
{
    PcTray *tray=context;
    if(tray && !tray->destroying) { ++tray->calls; notification_shell_cancel(&tray->shell,id); tray_leave(tray); }
}
void pc_tray_destroy(void *context)
{
    PcTray *tray=context;
    if(tray && !tray->destroying) {
        /* Shell IPC가 WPF 닫기에 재진입할 수 있다. Win32 소유자와 동일하게 가장
         * 바깥 request/message가 반환할 때까지 native 상태의 해제를 늦춘다. */
        tray->destroying=1; notification_shell_destroy(&tray->shell);
        if(!tray->calls) free(tray);
    }
}
