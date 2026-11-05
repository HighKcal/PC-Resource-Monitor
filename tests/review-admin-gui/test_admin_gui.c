/* Independent hidden-control regression. Product and existing tests are unchanged.
 * Existing ingress mocks prevent real collection/DB access. This executable has
 * the ordinary asInvoker test manifest; no product wmain is executed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static HWND observed_status;
static unsigned status_sets;
static int fail_get_text, fail_get_length, fail_set_text;
static int override_get_length=-1;
static BOOL WINAPI counted_set_text(HWND, LPCWSTR);
static int WINAPI controlled_get_text(HWND, LPWSTR, int);
static int WINAPI controlled_get_length(HWND);
#define SetWindowTextW counted_set_text
#define GetWindowTextW controlled_get_text
#define GetWindowTextLengthW controlled_get_length
#define main previous_native_review_main
#include "../review-native-ui/test_native_ui_review.c"
#undef main
#undef SetWindowTextW
#undef GetWindowTextW
#undef GetWindowTextLengthW

static BOOL WINAPI counted_set_text(HWND window, LPCWSTR text)
{
    if(window==observed_status) {
        ++status_sets;
        if(fail_set_text) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    }
    return SetWindowTextW(window,text);
}
static int WINAPI controlled_get_text(HWND window, LPWSTR text, int count)
{
    if(window==observed_status && fail_get_text) {
        if(count>0) text[0]=0;
        SetLastError(ERROR_ACCESS_DENIED); return 0;
    }
    return GetWindowTextW(window,text,count);
}
static int WINAPI controlled_get_length(HWND window)
{
    if(window==observed_status && fail_get_length) {
        SetLastError(ERROR_ACCESS_DENIED); return 0;
    }
    if(window==observed_status && override_get_length>=0) return override_get_length;
    return GetWindowTextLengthW(window);
}
static void one_tick(void) { SendMessageW(app.window,WM_TIMER,1,0); }
static void expect_current(const wchar_t *expected)
{
    wchar_t text[4096];
    CHECK(GetWindowTextW(app.status,text,(int)(sizeof(text)/sizeof(text[0])))==(int)wcslen(expected));
    CHECK(!wcscmp(text,expected));
}
static void expect_priority(const wchar_t *message)
{
    CHECK(SetWindowTextW(app.status,message)); status_sets=0;
    one_tick(); CHECK(status_sets==0); expect_current(message);
}
static void test_stale_timer(void)
{
    wchar_t stale[512],different[512],long_prefix[4096]; unsigned i; size_t length;
    MonitorRow preserved; uint64_t old_measured;
    init_app(); CHECK(create_hidden_window()!=NULL);
    if(!app.window) { clear_app(); return; }
    app.latest=test_view(1,0); consume_snapshot();
    observed_status=app.status;
    old_measured=monitor_utc_now()-UINT64_C(160000000);
    app.display->measured_utc_100ns=old_measured;
    preserved=app.display->rows[0];
    CHECK(SetWindowTextW(app.status,L"before first stale tick")); status_sets=0;
    one_tick(); CHECK(status_sets==1);
    CHECK(GetWindowTextW(app.status,stale,(int)(sizeof(stale)/sizeof(stale[0])))>0);
    CHECK(wcsstr(stale,L"표본 오래됨")!=NULL);
    CHECK(wcsstr(stale,L"15초")!=NULL);
    status_sets=0;
    for(i=0;i<100;++i) one_tick();
    CHECK(status_sets==0); expect_current(stale);
    CHECK(app.display->measured_utc_100ns==old_measured);
    CHECK(!memcmp(&preserved,&app.display->rows[0],sizeof(preserved)));

    /* Same length is insufficient; a long matching prefix must not be equality. */
    wcscpy_s(different,sizeof(different)/sizeof(different[0]),stale);
    different[0]=L'X';
    CHECK(SetWindowTextW(app.status,different)); status_sets=0;
    one_tick(); CHECK(status_sets==1); expect_current(stale);
    length=wcslen(stale);
    wcscpy_s(long_prefix,sizeof(long_prefix)/sizeof(long_prefix[0]),stale);
    for(i=(unsigned)length;i<3500;++i) long_prefix[i]=L'Y';
    long_prefix[3500]=0;
    CHECK(SetWindowTextW(app.status,long_prefix)); status_sets=0;
    one_tick(); CHECK(status_sets==1); expect_current(stale);
    /* Simulate a longer current value after the length query has completed. */
    CHECK(SetWindowTextW(app.status,long_prefix)); status_sets=0;
    override_get_length=(int)length; one_tick(); override_get_length=-1;
    CHECK(status_sets==1); expect_current(stale);

    /* Transient text queries must not silently latch a stale-cache hit. */
    fail_get_text=1; status_sets=0; one_tick();
    CHECK(status_sets==1); fail_get_text=0; expect_current(stale);
    fail_get_length=1; status_sets=0; one_tick();
    CHECK(status_sets==1); fail_get_length=0; expect_current(stale);
    CHECK(SetWindowTextW(app.status,L"another action status"));
    fail_set_text=1; status_sets=0; one_tick();
    CHECK(status_sets==1); expect_current(L"another action status");
    fail_set_text=0; one_tick(); CHECK(status_sets==2); expect_current(stale);
    status_sets=0; one_tick(); CHECK(status_sets==0);

    /* Timer optimization must preserve higher-priority and fresh states. */
    app.display->measured_utc_100ns=monitor_utc_now();
    expect_priority(L"fresh observation");
    app.display->measured_utc_100ns=old_measured;
    app.pause_requested=1; expect_priority(L"paused"); app.pause_requested=0;
    app.stopping=1; expect_priority(L"stopping"); app.stopping=0;
    app.worker_error=1; expect_priority(L"worker error"); app.worker_error=0;
    app.display->storage_error=1; expect_priority(L"storage error"); app.display->storage_error=0;
    app.display->collector_error=1; expect_priority(L"collector error"); app.display->collector_error=0;
    app.shown->session=0; expect_priority(L"old session waiting"); app.shown->session=app.session;
    app.synthetic=1; expect_priority(L"synthetic"); app.synthetic=0;
    app.ui_demo=1; expect_priority(L"demo"); app.ui_demo=0;
    status_sets=0; one_tick(); CHECK(status_sets==1); expect_current(stale);

    /* Actual command/feedback text is restored once, without a cached boolean. */
    SendMessageW(app.window,WM_COMMAND,ID_IGNORE,0);
    CHECK(GetWindowTextW(app.status,different,(int)(sizeof(different)/sizeof(different[0])))>0);
    CHECK(wcscmp(stale,different)!=0);
    status_sets=0; one_tick(); CHECK(status_sets==1); expect_current(stale);
    status_sets=0; one_tick(); CHECK(status_sets==0);
    observed_status=NULL; clear_app();
}
int main(void)
{
    (void)previous_native_review_main();
    test_stale_timer();
    printf("independent_admin_gui_review assertions=%u failures=%u\n",assertions,failures);
    return failures?1:0;
}
