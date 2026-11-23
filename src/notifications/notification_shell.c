/* UI 스레드에서 Windows tray·배너 요청과 콜백 수명을 관리한다. 정책 판단은 하지 않는다.
 * notification_shell_request → handle_message/tick → cancel/destroy에서 요청 ID와
 * 아이콘 수명을 읽는다. 반환된 요청 성공은 실제 사용자 배너 열람을 뜻하지 않는다. */
#include "notification_shell.h"

#include <math.h>
#include <shellapi.h>
#include <strsafe.h>

enum {
    IDLE_ICON_ID = 1,
    LAST_ICON_ID = 0xffff,
    MAINTENANCE_MS = 5000,
    EVENT_LIFETIME_MS = 30000,
    MENU_SHOW = 1,
    MENU_TOGGLE,
    MENU_EXIT
};

static const wchar_t *const metric_name[MONITOR_METRICS] = {L"CPU", L"RAM", L"읽기", L"쓰기"};
static const wchar_t *const metric_unit[MONITOR_METRICS] = {L"%", L"GB", L"MB/s", L"MB/s"};
static const double metric_divisor[MONITOR_METRICS] = {1.0, 1e9, 1e6, 1e6};

static NOTIFYICONDATAW icon_data(const NotificationShell *shell, UINT id)
{
    NOTIFYICONDATAW data = {0};
    data.cbSize = sizeof(data);
    data.hWnd = shell->owner;
    data.uID = id;
    return data;
}

static void release_tray(NotificationShell *shell)
{
    NOTIFYICONDATAW data = icon_data(shell, shell->icon_id);

    shell->owner = NULL;
    shell->icon_id = 0;
    shell->icon_versioned = false;
    if (data.uID) Shell_NotifyIconW(NIM_DELETE, &data);
}

static bool notify_icon(NotificationShell *shell, DWORD operation, NOTIFYICONDATAW *data)
{
    bool result;

    shell->in_shell_call = true;
    result = Shell_NotifyIconW(operation, data) != FALSE;
    shell->in_shell_call = false;
    if (result && operation == NIM_ADD) shell->icon_id = data->uID;
    if (result && operation == NIM_DELETE) shell->icon_id = 0;
    if (shell->closing) {
        release_tray(shell);
        return false;
    }
    return result;
}

static bool delete_icon(NotificationShell *shell)
{
    NOTIFYICONDATAW data = icon_data(shell, shell->icon_id);

    shell->icon_versioned = false;
    if (!shell->icon_id) return true;
    return notify_icon(shell, NIM_DELETE, &data);
}

static bool add_icon(NotificationShell *shell, UINT id)
{
    NOTIFYICONDATAW data = icon_data(shell, id);

    if (!shell->owner || !shell->icon || shell->icon_id) return false;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = shell->callback_message;
    data.hIcon = shell->icon;
    StringCchCopyW(data.szTip, ARRAYSIZE(data.szTip), L"내 PC 자원 모니터");
    if (!notify_icon(shell, NIM_ADD, &data)) return false;

    data = icon_data(shell, id);
    data.uVersion = NOTIFYICON_VERSION_4;
    shell->icon_versioned = notify_icon(shell, NIM_SETVERSION, &data);
    if (shell->icon_versioned) return true;
    delete_icon(shell);
    return false;
}

static void restore_idle_icon(NotificationShell *shell)
{
    if (shell->icon_versioned && shell->icon_id == IDLE_ICON_ID) return;
    if (delete_icon(shell)) add_icon(shell, IDLE_ICON_ID);
}

static uint64_t clear_event(NotificationShell *shell)
{
    uint64_t event_id = shell->active_event_id;

    shell->active_event_id = 0;
    shell->active_shown = false;
    return event_id;
}

static uint64_t retire_event(NotificationShell *shell)
{
    uint64_t event_id = clear_event(shell);

    restore_idle_icon(shell);
    return event_id;
}

static uint64_t recreate_tray(NotificationShell *shell, HWND shell_window)
{
    uint64_t event_id = clear_event(shell);

    /* 같은 Shell의 DPI broadcast만으로 삭제 실패한 아이콘이 없어졌다고 판단하지 않는다. */
    if (!delete_icon(shell) && shell_window == shell->shell_window) return event_id;
    shell->icon_id = 0;
    shell->shell_window = shell_window;
    add_icon(shell, IDLE_ICON_ID);
    return event_id;
}

static void notification_basename(const NotificationEvent *event, wchar_t name[33])
{
    wchar_t decoded[sizeof(event->name)];
    size_t first = 0, end = 0;
    int length;

    for (; end < sizeof(event->name) && event->name[end]; ++end) {
        char value = event->name[end];
        if (value == '\\' || value == '/' || value == ':') first = end + 1;
    }
    length = MultiByteToWideChar(CP_UTF8, 0, event->name + first, (int)(end - first),
                                 decoded, ARRAYSIZE(decoded));
    if (!length) {
        StringCchCopyW(name, 33, L"프로세스");
        return;
    }
    if (length > 32) {
        length = 31;
        if (IS_HIGH_SURROGATE(decoded[length - 1])) --length;
        decoded[length++] = L'…';
    }
    for (int i = 0; i < length; ++i) {
        wchar_t value = decoded[i];
        name[i] = (value < L' ' || (value >= 0x7f && value <= 0x9f)) ? L' ' : value;
    }
    name[length] = L'\0';
}

/* 정책이 복사한 완료창 근거를 배너로 바꾼다. 서로 다른 지표의 완료 시각을 하나로
 * 합치지 않으며, 구성원이 여럿이면 상세에서 각각 확인하도록 요약한다. */
static bool format_banner(const NotificationEvent *event, NOTIFYICONDATAW *data)
{
    wchar_t basename[33];
    const NotificationMember *representative = &event->members[0];
    SYSTEMTIME completed;
    unsigned first = 0, recurrences = 0;
    bool shared_end = true;
    wchar_t *end;
    size_t remaining;
    FILETIME time;

    while (first < MONITOR_METRICS && !(representative->metrics & (1u << first))) ++first;
    if (first == MONITOR_METRICS || !event->member_count || event->member_count > NOTIFICATION_MEMBER_LIMIT)
        return false;
    for (unsigned m = 0; m < event->member_count; ++m) {
        const NotificationMember *member = &event->members[m];
        for (unsigned i = 0; i < MONITOR_METRICS; ++i) {
            if (!(member->metrics & (1u << i))) continue;
            if (!isfinite(member->metric[i].current) || !isfinite(member->metric[i].threshold) ||
                !member->metric[i].window_end) return false;
            recurrences += member->recurrence_count[i];
        }
    }
    notification_basename(event, basename);
    StringCchCopyW(data->szInfoTitle, ARRAYSIZE(data->szInfoTitle), L"평소 상한을 지속적으로 초과한 변화 후보");
    if (event->member_count > 1 || event->omitted_count) {
        if (FAILED(StringCchPrintfExW(data->szInfo, ARRAYSIZE(data->szInfo), &end, &remaining, 0,
                                      L"%ls\n같은 실행 경로 · %u개 인스턴스 · 대표 PID %lu\n원인",
                                      basename, event->member_count, (unsigned long)representative->pid))) return false;
        for (unsigned i = 0; i < MONITOR_METRICS; ++i)
            if ((event->metrics & (1u << i)) && FAILED(StringCchPrintfExW(end, remaining, &end, &remaining, 0,
                                                                        L" %ls", metric_name[i]))) return false;
        if (event->omitted_count && FAILED(StringCchPrintfExW(end, remaining, &end, &remaining, 0,
                                                              L"\n추가 %u개 후보 근거 생략", event->omitted_count))) return false;
        if (recurrences && FAILED(StringCchPrintfExW(end, remaining, &end, &remaining, 0,
                                                     L"\n앞서 보류된 뒤 끝난 재발 지표 %u건", recurrences))) return false;
        return SUCCEEDED(StringCchCopyW(end, remaining, L"\n구성원별 완료창 근거는 클릭"));
    }
    for (unsigned i = first + 1; i < MONITOR_METRICS; ++i)
        if ((representative->metrics & (1u << i)) &&
            representative->metric[i].window_end != representative->metric[first].window_end) shared_end = false;
    time.dwLowDateTime = (DWORD)representative->metric[first].window_end;
    time.dwHighDateTime = (DWORD)(representative->metric[first].window_end >> 32);
    if (!FileTimeToSystemTime(&time, &completed)) return false;
    if (FAILED(StringCchPrintfExW(data->szInfo, ARRAYSIZE(data->szInfo), &end, &remaining, 0,
                                L"%ls\n완료 %04u-%02u-%02u %02u:%02u:%02u UTC", basename,
                                (unsigned)completed.wYear, (unsigned)completed.wMonth, (unsigned)completed.wDay,
                                (unsigned)completed.wHour, (unsigned)completed.wMinute, (unsigned)completed.wSecond))) {
        return false;
    }
    for (unsigned i = first; i < MONITOR_METRICS; ++i) {
        if (!(event->metrics & (1u << i))) continue;
        if (shared_end || i == first) {
            if (FAILED(StringCchPrintfExW(end, remaining, &end, &remaining, 0,
                                        L"\n%ls %.3g%ls > 상한 %.3g%ls", metric_name[i],
                                        representative->metric[i].current / metric_divisor[i], metric_unit[i],
                                        representative->metric[i].threshold / metric_divisor[i], metric_unit[i]))) return false;
        } else if (FAILED(StringCchPrintfExW(end, remaining, &end, &remaining, 0,
                                           L"\n%ls · 별도 완료창", metric_name[i]))) return false;
    }
    if (recurrences && FAILED(StringCchPrintfExW(end, remaining, &end, &remaining, 0,
                                                 L"\n앞서 보류된 뒤 끝난 재발 지표 %u건", recurrences))) return false;
    return shared_end || SUCCEEDED(StringCchCopyW(end, remaining, L"\n나머지 근거는 클릭"));
}

void notification_shell_init(NotificationShell *shell, HWND owner, UINT callback_message)
{
    *shell = (NotificationShell){0};
    shell->owner = owner;
    shell->callback_message = callback_message;
    shell->taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    shell->shell_window = GetShellWindow();
    shell->icon = LoadIconW(NULL, IDI_APPLICATION);
    shell->last_event_icon_id = IDLE_ICON_ID;
    add_icon(shell, IDLE_ICON_ID);
}

bool notification_shell_ready(const NotificationShell *shell)
{
    return shell->owner && !shell->active_event_id && !shell->in_shell_call && !shell->menu_open &&
           shell->icon_versioned && shell->icon_id == IDLE_ICON_ID &&
           shell->last_event_icon_id < LAST_ICON_ID;
}

/* UI 스레드에서 OS 알림 가능 상태를 확인한 뒤 배너를 요청한다. event는 호출 중만 빌린다.
 * 성공 시 ID·시작시각만 보관하며, 실패 시 가능하면 일반 트레이 아이콘을 복원한다. */
NotificationShellRequest notification_shell_request(NotificationShell *shell,
                                                      const NotificationEvent *event, uint64_t now_ms)
{
    QUERY_USER_NOTIFICATION_STATE state;
    NOTIFYICONDATAW data = icon_data(shell, shell->last_event_icon_id + 1u);

    if (!notification_shell_ready(shell)) return NOTIFICATION_SHELL_UNAVAILABLE;
    if (FAILED(SHQueryUserNotificationState(&state))) {
        ++shell->stats.rejected;
        return NOTIFICATION_SHELL_UNAVAILABLE;
    }
    if (state != QUNS_ACCEPTS_NOTIFICATIONS) {
        ++shell->stats.quiet;
        return NOTIFICATION_SHELL_QUIET;
    }
    if (!event->id || !format_banner(event, &data) || !delete_icon(shell)) {
        ++shell->stats.rejected;
        return NOTIFICATION_SHELL_REJECTED;
    }

    /* 제출이 실패해도 등록한 event 아이콘마다 새 callback ID를 사용한다. */
    shell->last_event_icon_id = data.uID;
    if (add_icon(shell, data.uID)) {
        data.uFlags = NIF_INFO | NIF_REALTIME;
        data.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        if (notify_icon(shell, NIM_MODIFY, &data)) {
            shell->active_event_id = event->id;
            shell->active_started_ms = now_ms;
            ++shell->stats.requested;
            return NOTIFICATION_SHELL_REQUESTED;
        }
    }
    restore_idle_icon(shell);
    ++shell->stats.rejected;
    return NOTIFICATION_SHELL_REJECTED;
}

static NotificationShellActionKind tray_menu(NotificationShell *shell, bool enabled)
{
    HWND owner = shell->owner;
    HMENU menu = CreatePopupMenu();
    POINT point;
    UINT selected = 0;

    if (!menu) return NOTIFICATION_SHELL_ACTION_NONE;
    if (AppendMenuW(menu, MF_STRING, MENU_SHOW, L"창 열기") &&
        AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED), MENU_TOGGLE, L"변화 알림") &&
        AppendMenuW(menu, MF_STRING, MENU_EXIT, L"종료") && GetCursorPos(&point)) {
        shell->menu_open = true;
        SetForegroundWindow(owner);
        selected = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                         point.x, point.y, owner, NULL);
        shell->menu_open = false;
        PostMessageW(owner, WM_NULL, 0, 0);
    }
    DestroyMenu(menu);
    switch (selected) {
    case MENU_SHOW: return NOTIFICATION_SHELL_ACTION_SHOW_WINDOW;
    case MENU_TOGGLE: return NOTIFICATION_SHELL_ACTION_TOGGLE;
    case MENU_EXIT: return NOTIFICATION_SHELL_ACTION_EXIT;
    }
    return NOTIFICATION_SHELL_ACTION_NONE;
}

/* 창 프로시저가 Shell 콜백을 전달한다. 현재 아이콘 ID와 맞는 콜백만 action으로 돌려주며
 * 클릭 결과의 사건 ID를 정책에 넘겨 당시 근거를 얻는 일은 호출자가 맡는다. */
bool notification_shell_handle_message(NotificationShell *shell, UINT message, WPARAM wp, LPARAM lp,
                                       bool enabled, NotificationShellAction *action)
{
    bool taskbar_created = shell->taskbar_created_message && message == shell->taskbar_created_message;
    UINT event;

    *action = (NotificationShellAction){0};
    if (!shell->owner || (!taskbar_created && message != shell->callback_message)) return false;
    if (shell->closing) return true;
    if (shell->in_shell_call) {
        /* 동기 Win32 콜백은 호출자의 상태 전이가 끝난 뒤 처리해야 한다. */
        PostMessageW(shell->owner, message, wp, lp);
        return true;
    }
    if (taskbar_created) {
        action->event_id = recreate_tray(shell, GetShellWindow());
        if (action->event_id) action->kind = NOTIFICATION_SHELL_ACTION_RETIRE;
        return true;
    }
    if (!shell->icon_versioned || HIWORD(lp) != shell->icon_id) return true;
    event = LOWORD(lp);
    switch (event) {
    case WM_CONTEXTMENU:
        if (!shell->menu_open) action->kind = tray_menu(shell, enabled);
        break;
    case NIN_SELECT:
    case NIN_KEYSELECT:
        action->kind = NOTIFICATION_SHELL_ACTION_SHOW_WINDOW;
        break;
    case NIN_BALLOONSHOW:
        if (shell->active_event_id && !shell->active_shown) {
            shell->active_shown = true;
            ++shell->stats.shown;
        }
        break;
    case NIN_BALLOONUSERCLICK:
        if (shell->active_event_id) {
            ++shell->stats.clicked;
            action->kind = NOTIFICATION_SHELL_ACTION_CLICK;
            action->event_id = retire_event(shell);
        }
        break;
    case NIN_BALLOONHIDE:
    case NIN_BALLOONTIMEOUT:
        if (shell->active_event_id) {
            action->kind = NOTIFICATION_SHELL_ACTION_RETIRE;
            action->event_id = retire_event(shell);
        }
        break;
    }
    return true;
}

uint64_t notification_shell_tick(NotificationShell *shell, uint64_t now_ms)
{
    HWND shell_window;

    if (!shell->owner || shell->in_shell_call) return 0;
    if (now_ms >= shell->maintenance_ms) {
        shell->maintenance_ms = now_ms + MAINTENANCE_MS;
        shell_window = GetShellWindow();
        if (shell_window != shell->shell_window) return recreate_tray(shell, shell_window);
        if (!shell->active_event_id) restore_idle_icon(shell);
    }
    if (shell->active_event_id && now_ms - shell->active_started_ms >= EVENT_LIFETIME_MS) return retire_event(shell);
    return 0;
}

void notification_shell_cancel(NotificationShell *shell, uint64_t event_id)
{
    if (event_id && event_id == shell->active_event_id) retire_event(shell);
}

void notification_shell_destroy(NotificationShell *shell)
{
    if (!shell->owner || shell->closing) return;
    shell->closing = true;
    clear_event(shell);
    /* Shell이 소유 결과를 돌려주기 전에 보낸 WM_CLOSE가 window procedure에 재진입할 수 있다. */
    if (!shell->in_shell_call) release_tray(shell);
}
