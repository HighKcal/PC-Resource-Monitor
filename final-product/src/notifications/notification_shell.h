#ifndef MONITOR_NOTIFICATION_SHELL_H
#define MONITOR_NOTIFICATION_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#include "notification_policy.h"

typedef enum {
    NOTIFICATION_SHELL_REQUESTED,
    NOTIFICATION_SHELL_UNAVAILABLE,
    NOTIFICATION_SHELL_QUIET,
    NOTIFICATION_SHELL_REJECTED
} NotificationShellRequest;

typedef enum {
    NOTIFICATION_SHELL_ACTION_NONE,
    NOTIFICATION_SHELL_ACTION_SHOW_WINDOW,
    NOTIFICATION_SHELL_ACTION_TOGGLE,
    NOTIFICATION_SHELL_ACTION_EXIT,
    NOTIFICATION_SHELL_ACTION_CLICK,
    NOTIFICATION_SHELL_ACTION_RETIRE
} NotificationShellActionKind;

typedef struct {
    NotificationShellActionKind kind;
    uint64_t event_id;
} NotificationShellAction;

typedef struct {
    uint64_t requested, shown, clicked, rejected, quiet;
} NotificationShellStats;

/* 소유 GUI 스레드에서 생성·사용·종료한다. owner는 빌린 창 핸들이고 icon은
 * LoadIcon으로 얻은 공유 시스템 아이콘이다. 트레이 등록은 destroy에서 정리한다. */
typedef struct {
    HWND owner, shell_window;
    HICON icon;
    UINT callback_message, taskbar_created_message;
    UINT icon_id, last_event_icon_id;
    uint64_t active_event_id, active_started_ms, maintenance_ms;
    bool icon_versioned, active_shown, in_shell_call, menu_open, closing;
    NotificationShellStats stats;
} NotificationShell;

/* 첫 트레이 등록이 실패해도 tick을 계속 호출해야 주기적으로 재시도한다. */
void notification_shell_init(NotificationShell *shell, HWND owner, UINT callback_message);
bool notification_shell_ready(const NotificationShell *shell);
NotificationShellRequest notification_shell_request(NotificationShell *shell,
                                                      const NotificationEvent *event, uint64_t now_ms);
bool notification_shell_handle_message(NotificationShell *shell, UINT message, WPARAM wp, LPARAM lp,
                                       bool enabled, NotificationShellAction *action);
/* 만료 또는 Shell 창 교체로 퇴역한 사건 ID를 반환한다. 호출자는 정책에서도 retire한다. */
uint64_t notification_shell_tick(NotificationShell *shell, uint64_t now_ms);
void notification_shell_cancel(NotificationShell *shell, uint64_t event_id);
void notification_shell_destroy(NotificationShell *shell);

#endif
