/* Shell API를 아래 fake 함수로 치환해 제품 notification_shell.c를 직접 검증한다.
 * 요청 수락·표시·클릭을 나누고, 실패 재시도·오래된 콜백·호출 중 종료를 대조한다.
 * 실제 트레이/배너를 만들지 않으며 각 사례가 destroy 뒤 fake 자원 소멸을 확인한다. */
#include <float.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "../../notifications/notification_shell.h"

enum { CALLBACK_MESSAGE = WM_APP + 7, TASKBAR_CREATED = 0xc123 };
static const HWND fake_owner = (HWND)(INT_PTR)100;
static HWND fake_shell_window;
static UINT fake_icon_id;
static unsigned fake_calls[NIM_SETVERSION + 1];
static DWORD fake_failed_operation;
static UINT fake_failed_icon;
static unsigned fake_failures_remaining;
static HRESULT fake_state_result;
static QUERY_USER_NOTIFICATION_STATE fake_state;
static NOTIFYICONDATAW fake_banner;
static NotificationShell *fake_callback_during_modify;
static LPARAM fake_deferred_callback;
static NotificationShell *fake_shell_to_destroy;
static DWORD fake_destroy_operation;
static unsigned checks, failures;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { ++failures; printf("line %d failed: %s\n", __LINE__, #condition); } \
} while (0)

static LPARAM callback_payload(UINT event, UINT icon_id)
{
    return (LPARAM)MAKELONG(event, icon_id);
}

static void fake_maybe_destroy(DWORD operation)
{
    NotificationShell *shell = fake_shell_to_destroy;

    if (shell && operation == fake_destroy_operation) {
        unsigned deletes = fake_calls[NIM_DELETE];
        fake_shell_to_destroy = NULL;
        notification_shell_destroy(shell);
        CHECK(shell->closing && shell->owner == fake_owner);
        CHECK(fake_calls[NIM_DELETE] == deletes);
    }
}

static BOOL WINAPI fake_notify_icon(DWORD operation, PNOTIFYICONDATAW data)
{
    CHECK(data->cbSize == sizeof(*data));
    CHECK(data->hWnd == fake_owner);
    ++fake_calls[operation];
    if (operation == fake_failed_operation && fake_failures_remaining &&
        (!fake_failed_icon || fake_failed_icon == data->uID)) {
        --fake_failures_remaining;
        return FALSE;
    }
    switch (operation) {
    case NIM_ADD:
        CHECK(fake_icon_id == 0);
        CHECK(data->uFlags == (NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP));
        CHECK(data->uCallbackMessage == CALLBACK_MESSAGE);
        if (fake_icon_id) return FALSE;
        fake_icon_id = data->uID;
        fake_maybe_destroy(operation);
        return TRUE;
    case NIM_SETVERSION:
        CHECK(data->uVersion == NOTIFYICON_VERSION_4);
        return data->uID == fake_icon_id;
    case NIM_DELETE:
        if (data->uID != fake_icon_id) return FALSE;
        fake_icon_id = 0;
        fake_maybe_destroy(operation);
        return TRUE;
    case NIM_MODIFY:
        CHECK(data->uID == fake_icon_id);
        CHECK(data->uFlags == (NIF_INFO | NIF_REALTIME));
        CHECK(data->dwInfoFlags == (NIIF_INFO | NIIF_RESPECT_QUIET_TIME));
        fake_banner = *data;
        fake_maybe_destroy(operation);
        if (fake_callback_during_modify) {
            NotificationShellAction action;
            CHECK(notification_shell_handle_message(fake_callback_during_modify, CALLBACK_MESSAGE, 0,
                                                     callback_payload(NIN_BALLOONSHOW, data->uID), true, &action));
            CHECK(action.kind == NOTIFICATION_SHELL_ACTION_NONE);
        }
        return TRUE;
    }
    return FALSE;
}

static HRESULT WINAPI fake_query_state(QUERY_USER_NOTIFICATION_STATE *state)
{
    *state = fake_state;
    return fake_state_result;
}

static HWND WINAPI fake_get_shell_window(void)
{
    return fake_shell_window;
}

static UINT WINAPI fake_register_message(LPCWSTR name)
{
    CHECK(wcscmp(name, L"TaskbarCreated") == 0);
    return TASKBAR_CREATED;
}

static HICON WINAPI fake_load_icon(HINSTANCE instance, LPCWSTR name)
{
    CHECK(!instance && name == IDI_APPLICATION);
    return (HICON)(INT_PTR)10;
}

static BOOL WINAPI fake_post_message(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    CHECK(window == fake_owner && message == CALLBACK_MESSAGE && wp == 0);
    fake_deferred_callback = lp;
    return TRUE;
}

#define Shell_NotifyIconW fake_notify_icon
#define SHQueryUserNotificationState fake_query_state
#define GetShellWindow fake_get_shell_window
#define RegisterWindowMessageW fake_register_message
#define LoadIconW fake_load_icon
#define PostMessageW fake_post_message
#include "../../notifications/notification_shell.c"

static void reset_fake(void)
{
    fake_shell_window = (HWND)(INT_PTR)200;
    fake_icon_id = 0;
    memset(fake_calls, 0, sizeof(fake_calls));
    fake_failed_operation = 0;
    fake_failed_icon = 0;
    fake_failures_remaining = 0;
    fake_state_result = S_OK;
    fake_state = QUNS_ACCEPTS_NOTIFICATIONS;
    fake_callback_during_modify = NULL;
    fake_deferred_callback = 0;
    fake_shell_to_destroy = NULL;
}

static NotificationEvent make_event(uint64_t id)
{
    NotificationEvent event = {0};
    FILETIME time;
    SYSTEMTIME completed = {0};
    uint64_t window_end;

    completed.wYear = 2026;
    completed.wMonth = 11;
    completed.wDay = 11;
    completed.wHour = 1;
    completed.wMinute = 42;
    completed.wSecond = 23;
    CHECK(SystemTimeToFileTime(&completed, &time));
    window_end = ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
    event.id = id;
    event.member_count = 1;
    event.metrics = event.members[0].metrics = (1u << MONITOR_METRICS) - 1u;
    strcpy_s(event.name, sizeof(event.name), "C:\\private\\repo\\my-process.exe");
    for (unsigned i = 0; i < MONITOR_METRICS; ++i) {
        event.members[0].metric[i].window_end = window_end;
        event.members[0].metric[i].current = 22.4 * metric_divisor[i];
        event.members[0].metric[i].threshold = 16.4 * metric_divisor[i];
    }
    return event;
}

static NotificationShellAction callback(NotificationShell *shell, UINT event, UINT id)
{
    NotificationShellAction action;

    CHECK(notification_shell_handle_message(shell, CALLBACK_MESSAGE, 0, callback_payload(event, id), true, &action));
    return action;
}

static void test_callbacks_and_text(void)
{
    NotificationShell shell;
    NotificationShellAction action;
    NotificationEvent event = make_event(888);

    reset_fake();
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    CHECK(notification_shell_ready(&shell) && fake_icon_id == 1);
    CHECK(callback(&shell, NIN_KEYSELECT, 1).kind == NOTIFICATION_SHELL_ACTION_SHOW_WINDOW);
    fake_callback_during_modify = &shell;
    CHECK(notification_shell_request(&shell, &event, 1000) == NOTIFICATION_SHELL_REQUESTED);
    fake_callback_during_modify = NULL;
    CHECK(fake_icon_id == 2 && !notification_shell_ready(&shell));
    CHECK(shell.stats.requested == 1 && shell.stats.shown == 0);
    CHECK(fake_deferred_callback == callback_payload(NIN_BALLOONSHOW, 2));
    CHECK(notification_shell_handle_message(&shell, CALLBACK_MESSAGE, 0, fake_deferred_callback, true, &action));
    CHECK(action.kind == NOTIFICATION_SHELL_ACTION_NONE && shell.stats.shown == 1);
    CHECK(callback(&shell, NIN_BALLOONSHOW, 2).kind == NOTIFICATION_SHELL_ACTION_NONE);
    CHECK(shell.stats.shown == 1);
    CHECK(wcsstr(fake_banner.szInfo, L"my-process.exe\n완료 2026-11-11 01:42:23 UTC"));
    CHECK(!wcsstr(fake_banner.szInfo, L"private"));
    CHECK(wcsstr(fake_banner.szInfo, L"CPU 22.4% > 상한 16.4%"));
    CHECK(wcsstr(fake_banner.szInfo, L"RAM 22.4GB > 상한 16.4GB"));
    CHECK(wcsstr(fake_banner.szInfo, L"읽기 22.4MB/s > 상한 16.4MB/s"));
    CHECK(wcsstr(fake_banner.szInfo, L"쓰기 22.4MB/s > 상한 16.4MB/s"));
    CHECK(callback(&shell, NIN_BALLOONUSERCLICK, 1).kind == NOTIFICATION_SHELL_ACTION_NONE);
    action = callback(&shell, NIN_BALLOONUSERCLICK, 2);
    CHECK(action.kind == NOTIFICATION_SHELL_ACTION_CLICK && action.event_id == 888);
    CHECK(shell.stats.clicked == 1 && notification_shell_ready(&shell) && fake_icon_id == 1);

    event.id = 889;
    event.members[0].metric[MONITOR_WRITE].window_end += UINT64_C(600000000);
    CHECK(notification_shell_request(&shell, &event, 2000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(fake_icon_id == 3);
    CHECK(wcsstr(fake_banner.szInfo, L"쓰기 · 별도 완료창"));
    CHECK(!wcsstr(fake_banner.szInfo, L"쓰기 22.4"));
    CHECK(callback(&shell, NIN_BALLOONUSERCLICK, 2).kind == NOTIFICATION_SHELL_ACTION_NONE);
    CHECK(callback(&shell, NIN_BALLOONTIMEOUT, 2).kind == NOTIFICATION_SHELL_ACTION_NONE);
    action = callback(&shell, NIN_BALLOONTIMEOUT, 3);
    CHECK(action.kind == NOTIFICATION_SHELL_ACTION_RETIRE && action.event_id == 889);
    CHECK(callback(&shell, NIN_BALLOONTIMEOUT, 3).kind == NOTIFICATION_SHELL_ACTION_NONE);
    CHECK(shell.stats.clicked == 1);
    notification_shell_destroy(&shell);
    CHECK(fake_icon_id == 0);
    notification_shell_destroy(&shell);
    CHECK(!notification_shell_handle_message(&shell, CALLBACK_MESSAGE, 0, 0, true, &action));
}

static void test_failure_and_recovery(void)
{
    NotificationShell shell;
    NotificationShellAction action;
    NotificationEvent event = make_event(7);
    unsigned adds;

    reset_fake();
    fake_failed_operation = NIM_ADD;
    fake_failures_remaining = 2;
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    CHECK(!notification_shell_ready(&shell));
    CHECK(notification_shell_tick(&shell, 5000) == 0 && fake_calls[NIM_ADD] == 2);
    CHECK(notification_shell_tick(&shell, 9999) == 0 && fake_calls[NIM_ADD] == 2);
    CHECK(notification_shell_tick(&shell, 10000) == 0 && notification_shell_ready(&shell));

    fake_state = QUNS_QUIET_TIME;
    adds = fake_calls[NIM_ADD];
    CHECK(notification_shell_request(&shell, &event, 10000) == NOTIFICATION_SHELL_QUIET);
    CHECK(shell.stats.quiet == 1 && fake_calls[NIM_ADD] == adds && !fake_calls[NIM_MODIFY]);
    fake_state_result = E_FAIL;
    CHECK(notification_shell_request(&shell, &event, 10000) == NOTIFICATION_SHELL_UNAVAILABLE);
    CHECK(shell.stats.rejected == 1 && fake_calls[NIM_ADD] == adds && !fake_calls[NIM_MODIFY]);
    fake_state = QUNS_ACCEPTS_NOTIFICATIONS;
    fake_state_result = S_OK;

    fake_failed_operation = NIM_SETVERSION;
    fake_failed_icon = 2;
    fake_failures_remaining = 1;
    CHECK(notification_shell_request(&shell, &event, 10000) == NOTIFICATION_SHELL_REJECTED);
    CHECK(shell.stats.rejected == 2 && fake_icon_id == 1 && shell.last_event_icon_id == 2);
    fake_failed_operation = NIM_MODIFY;
    fake_failed_icon = 3;
    fake_failures_remaining = 1;
    CHECK(notification_shell_request(&shell, &event, 10000) == NOTIFICATION_SHELL_REJECTED);
    CHECK(shell.stats.rejected == 3 && fake_icon_id == 1 && shell.last_event_icon_id == 3);
    CHECK(notification_shell_request(&shell, &event, 10000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(fake_icon_id == 4);
    CHECK(callback(&shell, NIN_BALLOONUSERCLICK, 3).kind == NOTIFICATION_SHELL_ACTION_NONE);
    notification_shell_cancel(&shell, 8);
    CHECK(fake_icon_id == 4);
    fake_failed_operation = NIM_DELETE;
    fake_failed_icon = 4;
    fake_failures_remaining = 1;
    notification_shell_cancel(&shell, 7);
    CHECK(fake_icon_id == 4 && !notification_shell_ready(&shell));
    CHECK(callback(&shell, NIN_BALLOONUSERCLICK, 4).kind == NOTIFICATION_SHELL_ACTION_NONE);
    CHECK(notification_shell_tick(&shell, 15000) == 0 && notification_shell_ready(&shell));
    CHECK(fake_icon_id == 1);

    CHECK(notification_shell_request(&shell, &event, 15000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(notification_shell_tick(&shell, 44999) == 0 && fake_icon_id == 5);
    CHECK(notification_shell_tick(&shell, 45000) == 7 && fake_icon_id == 1);

    CHECK(notification_shell_request(&shell, &event, 45000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(notification_shell_handle_message(&shell, TASKBAR_CREATED, 0, 0, true, &action));
    CHECK(action.kind == NOTIFICATION_SHELL_ACTION_RETIRE && action.event_id == 7 && fake_icon_id == 1);

    CHECK(notification_shell_request(&shell, &event, 45000) == NOTIFICATION_SHELL_REQUESTED);
    fake_failed_operation = NIM_DELETE;
    fake_failed_icon = fake_icon_id;
    fake_failures_remaining = 1;
    CHECK(notification_shell_handle_message(&shell, TASKBAR_CREATED, 0, 0, true, &action));
    CHECK(action.kind == NOTIFICATION_SHELL_ACTION_RETIRE && action.event_id == 7);
    CHECK(fake_icon_id == fake_failed_icon && !notification_shell_ready(&shell));
    CHECK(callback(&shell, NIN_BALLOONUSERCLICK, fake_icon_id).kind == NOTIFICATION_SHELL_ACTION_NONE);
    CHECK(notification_shell_tick(&shell, 50000) == 0 && notification_shell_ready(&shell));

    CHECK(notification_shell_request(&shell, &event, 50000) == NOTIFICATION_SHELL_REQUESTED);
    fake_icon_id = 0;
    fake_shell_window = (HWND)(INT_PTR)201;
    CHECK(notification_shell_tick(&shell, 55000) == 7 && fake_icon_id == 1);

    shell.last_event_icon_id = 0xfffe;
    CHECK(notification_shell_request(&shell, &event, 55000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(fake_icon_id == 0xffff);
    CHECK(callback(&shell, NIN_BALLOONHIDE, 0xffff).kind == NOTIFICATION_SHELL_ACTION_RETIRE);
    CHECK(!notification_shell_ready(&shell) && fake_icon_id == 1);
    CHECK(notification_shell_request(&shell, &event, 55000) == NOTIFICATION_SHELL_UNAVAILABLE);
    notification_shell_destroy(&shell);
    CHECK(fake_icon_id == 0 && shell.stats.shown == 0 && shell.stats.clicked == 0);
}

static void test_bounded_banner(void)
{
    NotificationShell shell;
    NotificationEvent event = make_event(99);

    reset_fake();
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    strcpy_s(event.name, sizeof(event.name), "C:\\private\\한글이름abcdefghijklmnopqrstuvwxyz0123456789.exe");
    for (unsigned i = 0; i < MONITOR_METRICS; ++i) {
        event.members[0].metric[i].current = DBL_MAX;
        event.members[0].metric[i].threshold = DBL_MAX / 2;
    }
    CHECK(notification_shell_request(&shell, &event, 1000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(wcslen(fake_banner.szInfo) < ARRAYSIZE(fake_banner.szInfo));
    CHECK(wcsstr(fake_banner.szInfo, L"한글이름abcdefghijklmnopqrstuvwxyz0…\n완료"));
    CHECK(!wcsstr(fake_banner.szInfo, L"private"));
    notification_shell_destroy(&shell);
    CHECK(fake_icon_id == 0);
}

static void test_bundle_text(void)
{
    NotificationShell shell;
    NotificationEvent event = make_event(121);
    event.member_count = 2;
    event.members[0].pid = 501;
    event.members[1] = event.members[0];
    event.members[1].pid = 502;
    event.members[0].metrics = 1u << MONITOR_RAM;
    event.members[1].metrics = 1u << MONITOR_WRITE;
    event.metrics = event.members[0].metrics | event.members[1].metrics;
    event.omitted_count = 3;
    event.omitted_metrics = 1u << MONITOR_CPU;
    event.members[0].recurrence_count[MONITOR_RAM] = 2;

    reset_fake();
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    CHECK(notification_shell_request(&shell, &event, 1000) == NOTIFICATION_SHELL_REQUESTED);
    CHECK(wcsstr(fake_banner.szInfo, L"2개 인스턴스 · 대표 PID 501"));
    CHECK(wcsstr(fake_banner.szInfo, L"원인 RAM 쓰기"));
    CHECK(wcsstr(fake_banner.szInfo, L"추가 3개 후보 근거 생략"));
    CHECK(wcsstr(fake_banner.szInfo, L"앞서 보류된 뒤 끝난 재발 지표 2건"));
    CHECK(!wcsstr(fake_banner.szInfo, L"22.4") && !wcsstr(fake_banner.szInfo, L"private"));
    notification_shell_destroy(&shell);
}

static void test_reentrant_close(void)
{
    NotificationShell shell;
    NotificationEvent event = make_event(99);

    reset_fake();
    fake_shell_to_destroy = &shell;
    fake_destroy_operation = NIM_ADD;
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    CHECK(!shell.owner && !shell.icon_id && !fake_icon_id);
    CHECK(fake_calls[NIM_ADD] == 1 && fake_calls[NIM_DELETE] == 1 && fake_calls[NIM_SETVERSION] == 0);

    reset_fake();
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    fake_shell_to_destroy = &shell;
    fake_destroy_operation = NIM_MODIFY;
    CHECK(notification_shell_request(&shell, &event, 1000) == NOTIFICATION_SHELL_REJECTED);
    CHECK(!shell.owner && !shell.icon_id && !fake_icon_id && !shell.active_event_id);
    CHECK(fake_calls[NIM_ADD] == 2 && fake_calls[NIM_DELETE] == 2 && fake_calls[NIM_MODIFY] == 1);
    CHECK(!shell.stats.requested);

    reset_fake();
    notification_shell_init(&shell, fake_owner, CALLBACK_MESSAGE);
    fake_shell_to_destroy = &shell;
    fake_destroy_operation = NIM_DELETE;
    CHECK(notification_shell_request(&shell, &event, 1000) == NOTIFICATION_SHELL_REJECTED);
    CHECK(!shell.owner && !shell.icon_id && !fake_icon_id && !shell.active_event_id);
    CHECK(fake_calls[NIM_ADD] == 1 && fake_calls[NIM_DELETE] == 1 && fake_calls[NIM_MODIFY] == 0);
}

int main(void)
{
    test_callbacks_and_text();
    test_failure_and_recovery();
    test_bounded_banner();
    test_bundle_text();
    test_reentrant_close();
    printf("%u checks, %u failures\n", checks, failures);
    return failures != 0;
}
