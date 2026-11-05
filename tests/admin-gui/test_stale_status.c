/* Hidden local control fixture; never starts the product worker or opens a DB. */
#define wmain product_wmain_not_called
#include "../../gui/monitor_gui.c"
#undef wmain

static unsigned checks, failures, set_calls;
static int fail_length, fail_text, forced_length = -1;
static WNDPROC original_status_proc;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %u: %s\n", (unsigned)__LINE__, #x); } } while (0)

static LRESULT CALLBACK fixture_status_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_SETTEXT) ++set_calls;
    if (message == WM_GETTEXTLENGTH) {
        if (fail_length) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0; }
        if (forced_length >= 0) return forced_length;
    }
    if (message == WM_GETTEXT && fail_text) {
        if (wp && lp) ((wchar_t *)lp)[0] = L'\0';
        SetLastError(ERROR_INVALID_WINDOW_HANDLE); return 0;
    }
    return CallWindowProcW(original_status_proc, window, message, wp, lp);
}

static void put(HWND window, const wchar_t *text)
{
    CHECK(SetWindowTextW(window, text)); set_calls = 0;
}

static void text_is(HWND window, const wchar_t *expected)
{
    wchar_t text[4096];
    CHECK(GetWindowTextW(window, text, (int)(sizeof(text) / sizeof(text[0]))) == (int)wcslen(expected));
    CHECK(!wcscmp(text, expected));
}

static void tick(void)
{
    window_proc(NULL, WM_TIMER, 1, 0);
}

int wmain(void)
{
    HWND status;
    MonitorSnapshot snapshot;
    GuiSnapshot view;
    wchar_t stale[256], changed[256], long_prefix[4096];
    unsigned i;
    status = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 100, 20,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(status != NULL); if (!status) return 1;
    original_status_proc = (WNDPROC)SetWindowLongPtrW(status, GWLP_WNDPROC, (LONG_PTR)fixture_status_proc);
    CHECK(original_status_proc != NULL);
    set_calls = 0; set_stale_status(status);
    CHECK(set_calls == 1);
    CHECK(GetWindowTextW(status, stale, (int)(sizeof(stale) / sizeof(stale[0]))) > 0);
    set_calls = 0;
    for (i = 0; i < 100; ++i) set_stale_status(status);
    CHECK(set_calls == 0); text_is(status, stale);

    /* A same-length replacement must not be mistaken for the stale message. */
    wcscpy_s(changed, sizeof(changed) / sizeof(changed[0]), stale); changed[0] = L'X';
    put(status, changed); set_stale_status(status);
    CHECK(set_calls == 1); text_is(status, stale);
    put(status, L"other status"); set_stale_status(status);
    CHECK(set_calls == 1); text_is(status, stale);

    /* Exact length avoids accepting a truncated equal prefix. */
    wcscpy_s(long_prefix, sizeof(long_prefix) / sizeof(long_prefix[0]), stale);
    for (i = (unsigned)wcslen(stale); i < 4095; ++i) long_prefix[i] = L'X';
    long_prefix[4095] = L'\0';
    put(status, long_prefix); set_stale_status(status);
    CHECK(set_calls == 1); text_is(status, stale);
    /* Simulate growth between WM_GETTEXTLENGTH and WM_GETTEXT. */
    put(status, long_prefix); forced_length = (int)wcslen(stale); set_stale_status(status); forced_length = -1;
    CHECK(set_calls == 1); text_is(status, stale);

    put(status, stale); fail_length = 1; set_stale_status(status); fail_length = 0;
    CHECK(set_calls == 1); text_is(status, stale);
    put(status, stale); fail_text = 1; set_stale_status(status); fail_text = 0;
    CHECK(set_calls == 1); text_is(status, stale);

    memset(&app, 0, sizeof(app)); memset(&snapshot, 0, sizeof(snapshot)); memset(&view, 0, sizeof(view));
    InitializeSRWLock(&app.lock); app.status = status; app.display = &snapshot; app.shown = &view;
    app.session = view.session = 1;
    snapshot.measured_utc_100ns = monitor_utc_now() - UINT64_C(300000000);
    put(status, L"prior"); tick(); CHECK(set_calls == 1); text_is(status, stale);
    tick(); CHECK(set_calls == 1); text_is(status, stale);
    put(status, L"feedback overwritten"); tick(); CHECK(set_calls == 1); text_is(status, stale);

    /* The existing stale gate must preserve fresh, paused, and failed states. */
    snapshot.measured_utc_100ns = monitor_utc_now();
    put(status, L"fresh"); tick(); CHECK(set_calls == 0); text_is(status, L"fresh");
    snapshot.measured_utc_100ns = monitor_utc_now() - UINT64_C(300000000);
    app.pause_requested = 1;
    put(status, L"paused"); tick(); CHECK(set_calls == 0); text_is(status, L"paused"); app.pause_requested = 0;
    app.worker_error = 1;
    put(status, L"worker error"); tick(); CHECK(set_calls == 0); text_is(status, L"worker error"); app.worker_error = 0;
    snapshot.collector_error = 1;
    put(status, L"collector error"); tick(); CHECK(set_calls == 0); text_is(status, L"collector error"); snapshot.collector_error = 0;
    snapshot.storage_error = 1;
    put(status, L"storage error"); tick(); CHECK(set_calls == 0); text_is(status, L"storage error"); snapshot.storage_error = 0;
    app.stopping = 1;
    put(status, L"stopping"); tick(); CHECK(set_calls == 0); text_is(status, L"stopping"); app.stopping = 0;
    view.session = 2;
    put(status, L"old session"); tick(); CHECK(set_calls == 0); text_is(status, L"old session"); view.session = 1;
    app.synthetic = 1;
    put(status, L"synthetic"); tick(); CHECK(set_calls == 0); text_is(status, L"synthetic"); app.synthetic = 0;
    app.ui_demo = 1;
    put(status, L"demo"); tick(); CHECK(set_calls == 0); text_is(status, L"demo"); app.ui_demo = 0;

    CHECK(DestroyWindow(status)); memset(&app, 0, sizeof(app));
    printf("stale status: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
