#include "../../../gui/program_research.h"
#include "../../../gui/native_program_info_browser.h"
static volatile LONG test_browser_mode,test_browser_hr,test_execute_delay,test_execute_expire,test_destroy_count;
/* 기본은 실제 COM apartment 초기화만 확인하는 오프라인 경계다.
 * 명시한 별도 probe만 일반 권한 Explorer 연결(1)/실제 문서 열기(2)를 수행한다. */
static HRESULT test_browser_open(const char *url)
{
    HRESULT hr;
    LONG mode=InterlockedCompareExchange(&test_browser_mode,0,0);
    if(mode==2) hr=program_info_browser_open(url);
    else if(mode==1) hr=program_info_browser_dispatch(url,0);
    else {
        hr=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
        if(SUCCEEDED(hr)) CoUninitialize();
    }
    InterlockedExchange(&test_browser_hr,(LONG)hr);
    return hr;
}
#define program_info_browser_open test_browser_open
#define pc_program_execute pc_program_execute_product
#define pc_program_destroy pc_program_destroy_product
#define pc_program_collect pc_program_collect_product
#include "../../../wpf/native/companion.c"
#undef pc_program_collect
#undef pc_program_execute
#undef pc_program_destroy
#undef program_info_browser_open
#include <shellapi.h>
static volatile LONG test_delay;
__declspec(dllexport) void pc_test_delay(unsigned milliseconds) { InterlockedExchange(&test_delay,(LONG)milliseconds); }
__declspec(dllexport) void *pc_program_collect(uint32_t pid,uint64_t creation,unsigned session,int synthetic,const char *path,HANDLE cancel)
{
    LONG delay=InterlockedExchange(&test_delay,0);
    if(delay>0) Sleep((DWORD)delay);
    return pc_program_collect_product(pid,creation,session,synthetic,path,cancel);
}
__declspec(dllexport) void pc_program_destroy(void *context)
{
    if(context) InterlockedIncrement(&test_destroy_count);
    pc_program_destroy_product(context);
}
__declspec(dllexport) int pc_program_execute(void *context,unsigned intent,int operation,unsigned source,HANDLE cancel)
{
    LONG delay=InterlockedExchange(&test_execute_delay,0);
    if(delay>0) Sleep((DWORD)delay);
    LONG invalidate=InterlockedExchange(&test_execute_expire,0);
    if(invalidate==1) ((PcProgram *)context)->local.expires_tick=0;
    else if(invalidate==2) ++((PcProgram *)context)->local.file_stamp.size;
    return pc_program_execute_product(context,intent,operation,source,cancel);
}
__declspec(dllexport) void pc_test_execute_delay(unsigned milliseconds,int expire)
{
    InterlockedExchange(&test_execute_delay,(LONG)milliseconds);
    InterlockedExchange(&test_execute_expire,expire);
}
__declspec(dllexport) int pc_test_destroy_count(void) { return (int)InterlockedCompareExchange(&test_destroy_count,0,0); }
__declspec(dllexport) void pc_test_browser_mode(int mode) { InterlockedExchange(&test_browser_mode,mode); InterlockedExchange(&test_browser_hr,E_PENDING); }
__declspec(dllexport) long pc_test_browser_hr(void) { return InterlockedCompareExchange(&test_browser_hr,0,0); }

/* 시험 전용 메모리 근거를 만든다. 이 DLL은 제품 배포물에 포함하지 않는다.
 * 자기 프로세스의 핸들/파일 stamp에 합성 메타데이터를 붙이며, 호출자는 pc_program_destroy로 해제한다.
 * 시험에서는 실제 외부 조회·브라우저 열기·번역 실행 함수를 호출하지 않는다. */
__declspec(dllexport) void *pc_test_fixture(void)
{
    PcProgram *p=calloc(1,sizeof(*p));
    DWORD length=PROGRAM_INFO_PATH_CAP;
    wchar_t path[PROGRAM_INFO_PATH_CAP];
    FILETIME created,exit_time,kernel,user;
    if(!p) return NULL;
    p->local.request.pid=GetCurrentProcessId(); p->local.request.session=7;
    (void)GetProcessTimes(GetCurrentProcess(),&created,&exit_time,&kernel,&user);
    p->local.request.creation=program_info_ft(created);
    (void)QueryFullProcessImageNameW(GetCurrentProcess(),0,path,&length);
    (void)program_info_utf8(path,p->local.request.path,sizeof(p->local.request.path));
    (void)DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&p->local.process_handle,0,FALSE,DUPLICATE_SAME_ACCESS);
    p->local.identity=PROGRAM_INFO_ID_VERIFIED; p->local.signature_verified=1;
    p->local.signature_code=ERROR_SUCCESS; p->local.metadata_source_verified=1;
    p->local.expires_tick=GetTickCount64()+60000;
    {
        ProgramInfoLocalFile opened;
        DWORD error=0;
        ProgramInfoRequest request=p->local.request;
        request.deadline_tick=GetTickCount64()+5000;
        if(program_info_open_local(path,&opened,&error)!=INVALID_HANDLE_VALUE) {
            (void)program_info_stamp(opened.file,&p->local.file_stamp);
            p->local.file_stamp.hash_valid=program_info_hash(opened.file,&request,p->local.file_stamp.sha256);
            program_info_close_local(&opened);
        }
    }
    strcpy_s(p->local.product,sizeof(p->local.product),"Companion Fixture");
    strcpy_s(p->local.publisher,sizeof(p->local.publisher),"Fixture Publisher");
    strcpy_s(p->local.company,sizeof(p->local.company),"Fixture Company");
    strcpy_s(p->local.original,sizeof(p->local.original),"Fixture.exe");
    strcpy_s(p->local.description,sizeof(p->local.description),"Public fixture description");
    program_info_role_text(&p->local);
    return p;
}
__declspec(dllexport) void pc_test_browser_source(void *context,int trusted)
{
    PcProgram *p=context;
    p->local.source_count=1;
    strcpy_s(p->local.sources[0].title,sizeof(p->local.sources[0].title),"Windows startup documentation");
    strcpy_s(p->local.sources[0].url,sizeof(p->local.sources[0].url),trusted?
        "https://support.microsoft.com/en-us/windows/experience/startup-boot/configure-startup-applications-in-windows":"https://example.com/untrusted");
}
__declspec(dllexport) void pc_test_research(void *context)
{
    PcProgram *p=context;
    p->has_research=1; p->research.request.request_id=123;
    p->research.status=PROGRAM_RESEARCH_COMPLETE;
    p->research.official_confirmed=p->research.product_matched=1;
    p->research.evidence_count=1; p->research.expires_tick=GetTickCount64()+60000;
    strcpy_s(p->research.url,sizeof(p->research.url),"https://example.com/fixture");
    strcpy_s(p->research.origin,sizeof(p->research.origin),"https://example.com");
    strcpy_s(p->research.evidence[0],sizeof(p->research.evidence[0]),"Companion Fixture is a local application for reading and explaining stored resource information.");
}
__declspec(dllexport) void pc_test_expire(void *context) { ((PcProgram *)context)->local.expires_tick=0; }
__declspec(dllexport) int pc_test_tray_stale(void)
{
    PcTray *tray=calloc(1,sizeof(*tray));
    int action=-1,handled; uint64_t id=999;
    if(!tray) return 0;
    tray->shell.owner=(HWND)(uintptr_t)1;
    tray->shell.callback_message=WM_APP+0x172;
    tray->shell.icon_versioned=true; tray->shell.icon_id=10; tray->shell.active_event_id=123;
    handled=pc_tray_message(tray,WM_APP+0x172,0,MAKELPARAM(NIN_BALLOONUSERCLICK,9),1,&action,&id);
    handled=handled && !action && !id && tray->shell.active_event_id==123;
    tray->shell.owner=NULL;
    pc_tray_destroy(tray); return handled;
}
