/* 기존 변화 사건 DB를 검토하는 별도 화면이다. 수집·현재 프로세스 판정은 수행하지 않고
 * worker가 이력 조회·독립 평가 저장·명시한 로컬 내보내기를 순차 실행한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "experiment.h"

#define ID_LIST 101
#define ID_VIEW 102
#define ID_METHOD 103
#define ID_REFRESH 104
#define ID_PREVIOUS 105
#define ID_NEXT 106
#define ID_START 107
#define ID_SAVE 108
#define ID_EXPORT 109
#define ID_JUDGMENT 110
#define ID_ACTION 111
#define ID_NOTE 112
#define ID_TRIAL 113
#define ID_ORDER 114
#define ID_BOOL_FIRST 120
#define PAGE_SIZE 200
#define WM_READY (WM_APP + 1)

typedef enum CommandKind { COMMAND_REFRESH, COMMAND_DETAIL, COMMAND_EVALUATE, COMMAND_EXPORT } CommandKind;
/* 큐에는 필요한 값과 문자열 배열을 복사한다. evaluation의 문자열 포인터는
 * UI 버퍼를 보관하지 않고 worker가 자기 Command의 문자열 배열로 다시 연결한다. */
typedef struct Command {
    CommandKind kind;
    unsigned offset, serial;
    int64_t decision, episode, activation_decision;
    uint64_t session, creation;
    uint32_t pid;
    unsigned metric;
    ExperimentMethod method;
    ExperimentEvaluation evaluation;
    char note[1024], trial[128], order[128], prefix[1024];
} Command;
/* 조회 배열을 함께 소유한다. latest에서 UI display로 인수하고 free_view로 일괄 해제한다. */
typedef struct View {
    ExperimentEpisode *episodes;
    size_t episode_count;
    ExperimentDecision *decisions[2];
    size_t count[2];
    ExperimentStats stats;
    unsigned offset;
} View;
typedef struct RowRef {
    int64_t decision, episode;
    ExperimentMethod method;
    size_t index;
} RowRef;
typedef struct App {
    HWND window, status, list, detail, view, method, judgment, action, note, trial, order;
    HWND checks[5], start;
    HANDLE worker, stop, wake;
    SRWLOCK lock;
    Command commands[16];
    unsigned command_count, offset, serial, detail_result_serial;
    View *latest, *display;
    char latest_status[1024], detail_result[20000], db[32768], export_path[32768];
    int dirty_status, dirty_detail, updating, stopping, worker_error;
    RowRef refs[PAGE_SIZE * 2];
    size_t row_count;
    int64_t selected_decision, selected_episode, timed_decision;
    LARGE_INTEGER frequency, started;
    uint64_t heartbeat, last_tick, max_gap, close_tick;
} App;
static App app;

static wchar_t *wide(const char *value)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    wchar_t *result = n ? malloc((size_t)n * sizeof(*result)) : NULL;
    if (result) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, n);
    return result;
}
static void text(HWND window, const char *value)
{
    wchar_t *w = wide(value); if (w) { SetWindowTextW(window, w); free(w); }
}
static int read_text(HWND window, char *value, size_t size)
{
    wchar_t buffer[1024];
    int n = GetWindowTextW(window, buffer, (int)(sizeof(buffer) / sizeof(buffer[0])));
    if (n < 0 || size > INT_MAX) return 0;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1, value, (int)size, NULL, NULL) != 0;
}
static const char *utc(uint64_t value, char *buffer, size_t size)
{
    FILETIME ft; SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)value; ft.dwHighDateTime = (DWORD)(value >> 32);
    if (!value || !FileTimeToSystemTime(&ft, &st)) snprintf(buffer, size, "not observed");
    else snprintf(buffer, size, "%04u-%02u-%02u %02u:%02u:%02u UTC", st.wYear,
        st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}
static void free_view(View *v)
{
    if (!v) return;
    experiment_episodes_free(v->episodes, v->episode_count);
    experiment_decisions_free(v->decisions[0], v->count[0]);
    experiment_decisions_free(v->decisions[1], v->count[1]); free(v);
}
static void worker_status(const char *value)
{
    AcquireSRWLockExclusive(&app.lock);
    snprintf(app.latest_status, sizeof(app.latest_status), "%s", value); app.dirty_status = 1;
    ReleaseSRWLockExclusive(&app.lock); PostMessageW(app.window, WM_READY, 0, 0);
}
static int queue(const Command *command)
{
    int ok = 0;
    AcquireSRWLockExclusive(&app.lock);
    if (app.command_count < 16) { app.commands[app.command_count++] = *command; ok = 1; }
    ReleaseSRWLockExclusive(&app.lock);
    if (ok) SetEvent(app.wake);
    else text(app.status, "Worker queue full. Action was not queued; retry after the current operation.");
    return ok;
}
static int refresh(Experiment *e, unsigned offset)
{
    View *v = calloc(1, sizeof(*v)), *old;
    if (!v) return 0;
    v->offset = offset;
    if (!experiment_stats(e, &v->stats) || !experiment_episodes(e, -1, PAGE_SIZE, offset, &v->episodes, &v->episode_count) ||
        !experiment_decisions(e, EXPERIMENT_PERSONALIZED, PAGE_SIZE, offset, &v->decisions[0], &v->count[0]) ||
        !experiment_decisions(e, EXPERIMENT_FIXED, PAGE_SIZE, offset, &v->decisions[1], &v->count[1])) {
        free_view(v); return 0;
    }
    AcquireSRWLockExclusive(&app.lock); old = app.latest; app.latest = v;
    ReleaseSRWLockExclusive(&app.lock); free_view(old); PostMessageW(app.window, WM_READY, 0, 0); return 1;
}
/* 보관 ID만 믿지 않고 선택한 세션·PID+생성시각·지표·정책·사건을 다시 대조한다.
 * 만료·불일치 때 다른 decision을 대신 보여 주지 않는다. */
static void decision_text(Experiment *e, const Command *target, int64_t id, char *buffer, size_t size)
{
    ExperimentDecision *d = NULL;
    char explanation[7168];
    int written;
    if (id <= 0) written = snprintf(buffer, size, "Unavailable: no retained decision reference.");
    else if (!experiment_get_decision(e, id, &d))
        written = snprintf(buffer, size, "Unavailable or expired: %s", experiment_error(e));
    else if (d->session != target->session || d->pid != target->pid || d->creation != target->creation ||
        d->metric != target->metric || d->method != target->method || (target->episode && d->episode_id != target->episode))
        written = snprintf(buffer, size, "Unavailable: retained decision target does not match the selected event or comparison.");
    else if (!experiment_explain(d, monitor_utc_now(), explanation, sizeof(explanation)))
        written = snprintf(buffer, size, "Unavailable: decision %lld could not be rendered within the bounded display.", (long long)id);
    else written = snprintf(buffer, size, "Decision %lld | %s | %s\r\n%s", (long long)d->id,
        experiment_method_name(d->method), monitor_metric_name(d->metric), explanation);
    experiment_decisions_free(d, d ? 1 : 0);
    if (written < 0 || (size_t)written >= size)
        snprintf(buffer, size, "Unavailable: evidence display exceeded its bound; use the local export.");
}
static DWORD WINAPI worker(void *unused)
{
    Experiment *e = NULL;
    char error[512];
    (void)unused;
    if (!experiment_open_review(app.db, &e, error, sizeof(error))) {
        worker_status(error); app.worker_error = 1; return 1;
    }
    if (!refresh(e, 0)) { worker_status(experiment_error(e)); app.worker_error = 1; }
    for (;;) {
        Command c = {0};
        int available;
        AcquireSRWLockExclusive(&app.lock);
        available = app.command_count > 0;
        if (available) {
            c = app.commands[0]; --app.command_count;
            memmove(app.commands, app.commands + 1, app.command_count * sizeof(app.commands[0]));
        } else ResetEvent(app.wake);
        ReleaseSRWLockExclusive(&app.lock);
        /* 닫기를 요청해도 이미 큐에 복사한 작업은 끝내고, 큐가 비면 DB를 닫는다. */
        if (!available) {
            HANDLE waits[2] = { app.stop, app.wake };
            if (WaitForSingleObject(app.stop, 0) == WAIT_OBJECT_0) break;
            WaitForMultipleObjects(2, waits, FALSE, 250); continue;
        }
        if (c.kind == COMMAND_REFRESH) {
            if (!refresh(e, c.offset)) worker_status(experiment_error(e));
        } else if (c.kind == COMMAND_DETAIL) {
            char latest[8192], activation[8192], combined[20000];
            int written;
            decision_text(e, &c, c.decision, latest, sizeof(latest));
            if (!c.episode) written = snprintf(combined, sizeof(combined), "%s", latest);
            else if (c.activation_decision == c.decision) written = snprintf(combined, sizeof(combined),
                "%sActivation evidence:\r\n%s\r\n\r\nLatest or recovery evidence: the same decision as activation above; not repeated.", c.prefix, latest);
            else {
                decision_text(e, &c, c.activation_decision, activation, sizeof(activation));
                written = snprintf(combined, sizeof(combined), "%sActivation evidence:\r\n%s\r\n\r\nLatest or recovery evidence:\r\n%s",
                    c.prefix, activation, latest);
            }
            if (written < 0 || (size_t)written >= sizeof(combined)) snprintf(combined, sizeof(combined),
                "Evidence display exceeded its bound. Activation and latest evidence were not substituted; inspect the explicit local export.");
            AcquireSRWLockExclusive(&app.lock);
            memcpy(app.detail_result, combined, strlen(combined) + 1);
            app.detail_result_serial = c.serial; app.dirty_detail = 1;
            ReleaseSRWLockExclusive(&app.lock); PostMessageW(app.window, WM_READY, 0, 0);
        } else if (c.kind == COMMAND_EVALUATE) {
            c.evaluation.note = c.note; c.evaluation.trial_id = c.trial; c.evaluation.order_id = c.order;
            if (experiment_evaluate(e, &c.evaluation)) {
                if (!refresh(e, c.offset)) worker_status(experiment_error(e));
                else worker_status("Evaluation saved separately. Comparison facts, suppression and Baseline are unchanged. Timing is UI task time only.");
            } else worker_status(experiment_error(e));
        } else if (c.kind == COMMAND_EXPORT) {
            if (experiment_export(e, app.export_path)) worker_status("Local CSV exported to the explicit --export path; no data uploaded.");
            else worker_status(experiment_error(e));
        }
    }
    experiment_close(e); return 0;
}
static void list_text(int row, int column, const char *value)
{
    wchar_t *w = wide(value); if (w) { ListView_SetItemText(app.list, row, column, w); free(w); }
}
static int view_windows(void) { return SendMessageW(app.view, CB_GETCURSEL, 0, 0) == 1; }
static int method_filter(void) { return (int)SendMessageW(app.method, CB_GETCURSEL, 0, 0) - 1; }
static void cancel_timer(void)
{
    app.timed_decision = 0; SetWindowTextW(app.start, L"Start task timer");
}
static void clear_response(void)
{
    unsigned i;
    SendMessageW(app.judgment, CB_SETCURSEL, 0, 0); SendMessageW(app.action, CB_SETCURSEL, 0, 0);
    for (i = 0; i < 5; ++i) SendMessageW(app.checks[i], CB_SETCURSEL, 0, 0);
    SetWindowTextW(app.note, L"");
}
static void show_selected(void)
{
    int index = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
    Command c;
    RowRef *ref;
    if (index < 0 || (size_t)index >= app.row_count || !app.display) {
        ++app.serial; /* 이미 요청한 상세 응답이 늦게 도착해도 선택 해제 화면에 붙지 않게 한다. */
        if (app.selected_decision || app.selected_episode) clear_response();
        app.selected_decision = app.selected_episode = 0; cancel_timer();
        text(app.detail, "Select an event, or switch to All comparison windows to inspect learning, missing and noncandidate evidence.\r\n"
            "Both policies use the same observed windows and this same explanation. The fixed rule is provisional.\r\n"
            "Evaluation is separate from Ignore/Expected and never trains the Baseline. No user benefit or accuracy has been measured."); return;
    }
    ref = &app.refs[index];
    if (ref->decision != app.selected_decision || ref->episode != app.selected_episode) {
        cancel_timer(); clear_response(); text(app.detail, "Loading selected evidence from the local worker...");
    }
    app.selected_decision = ref->decision; app.selected_episode = ref->episode;
    memset(&c, 0, sizeof(c)); c.kind = COMMAND_DETAIL; c.decision = ref->decision; c.serial = ++app.serial;
    if (ref->episode) {
        ExperimentEpisode *p = &app.display->episodes[ref->index];
        char first[64], activated[64], last[64], ended[64];
        int written;
        c.episode = ref->episode; c.activation_decision = p->activation_decision_id;
        c.session = p->session; c.pid = p->pid; c.creation = p->creation; c.metric = p->metric; c.method = p->method;
        written = snprintf(c.prefix, sizeof(c.prefix), "EVENT %lld | %s | %s\r\n"
            "First observed qualifying window: %s; third-window activation: %s\r\n"
            "Last observed: %s; ended: %s; qualifying windows=%u\r\n"
            "End reason: %s. These are observation times, not cause onset.\r\n\r\n",
            (long long)p->id, experiment_method_name(p->method), experiment_episode_state_name(p->state),
            utc(p->first_utc, first, sizeof(first)), utc(p->activated_utc, activated, sizeof(activated)),
            utc(p->last_utc, last, sizeof(last)), utc(p->ended_utc, ended, sizeof(ended)),
            p->qualifying_windows, p->end_reason ? p->end_reason : "still active in recorded data");
        if (written < 0 || (size_t)written >= sizeof(c.prefix)) snprintf(c.prefix, sizeof(c.prefix),
            "EVENT %lld; event header unavailable within display bound. Activation and latest evidence follow separately.\r\n\r\n", (long long)p->id);
    } else {
        ExperimentDecision *d = &app.display->decisions[ref->method][ref->index];
        c.session = d->session; c.pid = d->pid; c.creation = d->creation; c.metric = d->metric; c.method = d->method;
    }
    if (!queue(&c)) { ++app.serial; text(app.detail, "Detail was not queued. Select the row again after the worker finishes."); }
}
static void add_row(const RowRef *ref, const char *name, uint32_t pid, unsigned metric,
    const char *state, uint64_t first, uint64_t last)
{
    LVITEMW item;
    char value[96], first_text[64], last_text[64];
    int index = (int)app.row_count;
    if (app.row_count >= sizeof(app.refs) / sizeof(app.refs[0])) return;
    app.refs[app.row_count++] = *ref;
    memset(&item, 0, sizeof(item)); item.mask = LVIF_TEXT; item.iItem = index; item.pszText = wide(name);
    ListView_InsertItem(app.list, &item); free(item.pszText);
    snprintf(value, sizeof(value), "%lld", (long long)(ref->episode ? ref->episode : ref->decision)); list_text(index, 1, value);
    list_text(index, 2, experiment_method_name(ref->method));
    snprintf(value, sizeof(value), "%lu", (unsigned long)pid); list_text(index, 3, value);
    list_text(index, 4, monitor_metric_name(metric)); list_text(index, 5, state);
    list_text(index, 6, utc(first, first_text, sizeof(first_text))); list_text(index, 7, utc(last, last_text, sizeof(last_text)));
    if ((ref->episode && ref->episode == app.selected_episode) ||
        (!ref->episode && !app.selected_episode && ref->decision == app.selected_decision))
        ListView_SetItemState(app.list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
static void render(void)
{
    size_t i;
    int method = method_filter(), windows = view_windows();
    char status[2048], source[768], latest_utc[64];
    if (!app.display) return;
    app.updating = 1; SendMessageW(app.list, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(app.list); app.row_count = 0;
    if (windows) {
        unsigned m;
        for (m = 0; m < 2; ++m) if (method < 0 || method == (int)m) {
            for (i = 0; i < app.display->count[m]; ++i) {
                ExperimentDecision *d = &app.display->decisions[m][i];
                RowRef ref = { d->id, 0, d->method, i };
                add_row(&ref, d->name, d->pid, d->metric, monitor_state_name(d->report.state), d->start_utc, d->end_utc);
            }
        }
    } else for (i = 0; i < app.display->episode_count; ++i) {
        ExperimentEpisode *p = &app.display->episodes[i];
        RowRef ref = { p->last_decision_id, p->id, p->method, i };
        if (method < 0 || method == (int)p->method)
            add_row(&ref, p->name, p->pid, p->metric, experiment_episode_state_name(p->state), p->first_utc, p->last_utc);
    }
    SendMessageW(app.list, WM_SETREDRAW, TRUE, 0); InvalidateRect(app.list, NULL, TRUE); app.updating = 0;
    if (app.display->stats.latest_source_available) snprintf(source, sizeof(source),
        "LATEST SOURCE RAW %s: status=%d rows=%llu/%llu; identity NA=%llu path NA=%llu metric NA CPU/RAM/read/write=%llu/%llu/%llu/%llu (overlap; separate from comparison states)",
        utc(app.display->stats.latest_source_utc, latest_utc, sizeof(latest_utc)), app.display->stats.latest_source_status,
        (unsigned long long)app.display->stats.latest_source_raw_rows, (unsigned long long)app.display->stats.latest_source_rows,
        (unsigned long long)app.display->stats.latest_source_identity_unavailable, (unsigned long long)app.display->stats.latest_source_path_unavailable,
        (unsigned long long)app.display->stats.latest_source_metric_missing[0], (unsigned long long)app.display->stats.latest_source_metric_missing[1],
        (unsigned long long)app.display->stats.latest_source_metric_missing[2], (unsigned long long)app.display->stats.latest_source_metric_missing[3]);
    else snprintf(source, sizeof(source), "LATEST SOURCE RAW unavailable/expired/incomplete or replay; missing counts NA. Copied historical comparisons remain separate.");
    snprintf(status, sizeof(status), "%s | %s / %s | source pair %s | %s | offset %u, %zu displayed\r\n"
        "retained windows=%llu%s events=%llu (active %llu, recovered %llu, unknown %llu), evaluations=%llu. History is not current process state.\r\n%s",
        app.display->stats.synthetic ? "SYNTHETIC" : "REAL LOCAL", EXPERIMENT_POLICY_VERSION, EXPERIMENT_TEMPLATE_VERSION,
        app.display->stats.source_available ? "available" : "missing/expired; copied evidence only",
        !app.display->stats.replay_only ? "recorded live/demo history" : app.display->stats.replay_comparator_recomputed ?
            "replay: fixed recomputed, personal recorded" : "replay: expired-prefix projection only", app.display->offset, app.row_count,
        (unsigned long long)app.display->stats.windows, app.display->stats.windows ? "" : " (none completed; not normal)", (unsigned long long)app.display->stats.episodes,
        (unsigned long long)app.display->stats.active, (unsigned long long)app.display->stats.recovered,
        (unsigned long long)app.display->stats.unknown, (unsigned long long)app.display->stats.evaluations, source);
    text(app.status, status);
    SetWindowTextW(app.window, app.display->stats.synthetic ? L"SYNTHETIC - Local change-event prototype" : L"Local change-event prototype - history review");
    show_selected();
}
/* 잠금 안에서 결과를 복사·분리한 뒤 UI를 갱신한다. 상세 serial은 선택 요청 순서이며
 * 늦게 온 응답이 현재 선택의 설명을 덮는 것을 막는다. */
static void consume(void)
{
    View *v;
    char status[1024], detail[20000];
    int ds, dd;
    unsigned serial;
    AcquireSRWLockExclusive(&app.lock);
    v = app.latest; app.latest = NULL;
    ds = app.dirty_status; dd = app.dirty_detail; serial = app.detail_result_serial;
    snprintf(status, sizeof(status), "%s", app.latest_status); snprintf(detail, sizeof(detail), "%s", app.detail_result);
    app.dirty_status = app.dirty_detail = 0;
    ReleaseSRWLockExclusive(&app.lock);
    if (v) { free_view(app.display); app.display = v; app.offset = v->offset; render(); }
    if (ds) text(app.status, status);
    if (dd && serial == app.serial) text(app.detail, detail);
}
static HWND control(HWND parent, const wchar_t *klass, const wchar_t *caption, DWORD style, int id)
{
    HWND result = CreateWindowW(klass, caption, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(result, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE); return result;
}
static HWND combo(HWND parent, int id, const wchar_t *const *items, unsigned count)
{
    unsigned i;
    HWND c = control(parent, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, id);
    for (i = 0; i < count; ++i) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageW(c, CB_SETCURSEL, 0, 0); return c;
}
static void layout(void)
{
    RECT r;
    int width, height, list_height, detail_y, detail_height, y;
    unsigned i;
    GetClientRect(app.window, &r); width = r.right; height = r.bottom;
    list_height = (height - 365) / 3; if (list_height < 110) list_height = 110;
    MoveWindow(app.status, 10, 8, width - 20, 70, TRUE);
    MoveWindow(app.view, 10, 82, 210, 150, TRUE); MoveWindow(app.method, 230, 82, 190, 150, TRUE);
    MoveWindow(GetDlgItem(app.window, ID_REFRESH), 430, 80, 85, 28, TRUE);
    MoveWindow(GetDlgItem(app.window, ID_PREVIOUS), 525, 80, 85, 28, TRUE);
    MoveWindow(GetDlgItem(app.window, ID_NEXT), 620, 80, 85, 28, TRUE);
    MoveWindow(GetDlgItem(app.window, ID_EXPORT), 715, 80, 160, 28, TRUE);
    MoveWindow(app.list, 10, 116, width - 20, list_height, TRUE);
    detail_y = 126 + list_height; detail_height = height - detail_y - 218; if (detail_height < 100) detail_height = 100;
    MoveWindow(app.detail, 10, detail_y, width - 20, detail_height, TRUE);
    y = detail_y + detail_height + 8;
    MoveWindow(GetDlgItem(app.window, 200), 10, y, width - 20, 20, TRUE); y += 24;
    MoveWindow(app.judgment, 10, y, 195, 160, TRUE); MoveWindow(app.action, 215, y, 220, 160, TRUE);
    MoveWindow(app.start, 445, y - 2, 160, 28, TRUE); MoveWindow(GetDlgItem(app.window, ID_SAVE), 615, y - 2, 180, 28, TRUE);
    y += 34;
    for (i = 0; i < 5; ++i) MoveWindow(app.checks[i], 10 + (int)i * 205, y, 195, 160, TRUE);
    y += 35;
    MoveWindow(GetDlgItem(app.window, 201), 10, y, 55, 20, TRUE); MoveWindow(app.trial, 65, y - 2, 160, 24, TRUE);
    MoveWindow(GetDlgItem(app.window, 202), 235, y, 65, 20, TRUE); MoveWindow(app.order, 300, y - 2, 160, 24, TRUE);
    MoveWindow(GetDlgItem(app.window, 203), 470, y, 45, 20, TRUE); MoveWindow(app.note, 515, y - 2, width - 525, 24, TRUE);
    y += 32; MoveWindow(GetDlgItem(app.window, 204), 10, y, width - 20, 44, TRUE);
}
static int answer(HWND combo_window)
{
    int selected = (int)SendMessageW(combo_window, CB_GETCURSEL, 0, 0);
    return selected == 1 ? 1 : selected == 2 ? 0 : -1;
}
/* 평가는 비교 근거와 별도로 저장한다. QPC 경과초는 사용자가 시작한 화면 작업 시간이며
 * 판정 정확도·성능 향상·사용자 효용의 측정값으로 해석하지 않는다. */
static void save_evaluation(void)
{
    Command c;
    LARGE_INTEGER ended;
    char note[768], action[128];
    int judgment = (int)SendMessageW(app.judgment, CB_GETCURSEL, 0, 0);
    if (!app.selected_decision || judgment < 1) { text(app.status, "Select evidence and choose an evaluation judgment before saving."); return; }
    memset(&c, 0, sizeof(c)); c.kind = COMMAND_EVALUATE; c.offset = app.offset;
    c.evaluation.decision_id = app.selected_decision; c.evaluation.episode_id = app.selected_episode;
    c.evaluation.judgment = (ExperimentJudgment)judgment;
    c.evaluation.understood = answer(app.checks[0]); c.evaluation.target_correct = answer(app.checks[1]);
    c.evaluation.time_correct = answer(app.checks[2]); c.evaluation.metric_correct = answer(app.checks[3]);
    c.evaluation.next_check = answer(app.checks[4]); c.evaluation.elapsed_seconds = -1;
    if (!read_text(app.note, note, sizeof(note)) || !read_text(app.action, action, sizeof(action)) ||
        !read_text(app.trial, c.trial, sizeof(c.trial)) || !read_text(app.order, c.order, sizeof(c.order))) {
        text(app.status, "Evaluation text could not be encoded; shorten the note/IDs."); return;
    }
    if (!c.trial[0]) { text(app.status, "Trial ID is required. Use ad-hoc for an unscheduled local review."); return; }
    snprintf(c.note, sizeof(c.note), "next_action=%s; %s", action, note);
    if (app.timed_decision == app.selected_decision && QueryPerformanceCounter(&ended) && ended.QuadPart >= app.started.QuadPart)
        c.evaluation.elapsed_seconds = (double)(ended.QuadPart - app.started.QuadPart) / (double)app.frequency.QuadPart;
    if (queue(&c)) { cancel_timer(); text(app.status, "Evaluation queued. The worker will confirm whether it was saved."); }
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_CREATE: {
        const wchar_t *views[] = { L"Events", L"All comparison windows" };
        const wchar_t *methods[] = { L"Both policies", L"Personalized Baseline", L"Fixed provisional rule" };
        const wchar_t *judgments[] = { L"Evaluation: choose...", L"Newly useful", L"Already known", L"Unnecessary check", L"Unsure" };
        const wchar_t *actions[] = { L"Next check: not chosen", L"Check app's current work", L"Compare after work ends", L"Inspect Task Manager", L"Review local app logs", L"Need more evidence" };
        const wchar_t *columns[] = { L"Process", L"Ref", L"Policy", L"PID", L"Metric", L"State", L"First/window start UTC", L"Last/window end UTC" };
        const int widths[] = { 220, 70, 150, 65, 140, 105, 210, 210 };
        const wchar_t *check_labels[5][3] = {
            { L"Understood: unknown", L"Understood: yes", L"Understood: no" },
            { L"Target found: unknown", L"Target found: yes", L"Target found: no" },
            { L"Time found: unknown", L"Time found: yes", L"Time found: no" },
            { L"Metric found: unknown", L"Metric found: yes", L"Metric found: no" },
            { L"Next check succeeded: unknown", L"Next check succeeded: yes", L"Next check succeeded: no" }
        };
        unsigned i;
        app.window = window;
        app.status = control(window, L"STATIC", L"Opening local history worker...", 0, 0);
        app.view = combo(window, ID_VIEW, views, 2); app.method = combo(window, ID_METHOD, methods, 3);
        control(window, L"BUTTON", L"Refresh", WS_TABSTOP, ID_REFRESH);
        control(window, L"BUTTON", L"Previous", WS_TABSTOP, ID_PREVIOUS);
        control(window, L"BUTTON", L"Next page", WS_TABSTOP, ID_NEXT);
        control(window, L"BUTTON", L"Export local CSV", WS_TABSTOP, ID_EXPORT);
        EnableWindow(GetDlgItem(window, ID_EXPORT), app.export_path[0] != 0);
        app.list = control(window, WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | WS_BORDER, ID_LIST);
        ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        for (i = 0; i < 8; ++i) {
            LVCOLUMNW c; memset(&c, 0, sizeof(c)); c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (wchar_t *)columns[i]; c.cx = widths[i]; ListView_InsertColumn(app.list, (int)i, &c);
        }
        app.detail = control(window, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | WS_BORDER, 0);
        SendMessageW(app.detail, EM_SETLIMITTEXT, 24000, 0);
        control(window, L"STATIC", L"Separate evaluation: self-reported answers, not verified truth or training labels", 0, 200);
        app.judgment = combo(window, ID_JUDGMENT, judgments, 5); app.action = combo(window, ID_ACTION, actions, 6);
        app.start = control(window, L"BUTTON", L"Start task timer", WS_TABSTOP, ID_START);
        control(window, L"BUTTON", L"Save evaluation", WS_TABSTOP, ID_SAVE);
        for (i = 0; i < 5; ++i) app.checks[i] = combo(window, ID_BOOL_FIRST + (int)i, check_labels[i], 3);
        control(window, L"STATIC", L"Trial ID", 0, 201); app.trial = control(window, L"EDIT", L"ad-hoc", WS_TABSTOP | WS_BORDER, ID_TRIAL);
        control(window, L"STATIC", L"Order ID", 0, 202); app.order = control(window, L"EDIT", L"unassigned", WS_TABSTOP | WS_BORDER, ID_ORDER);
        control(window, L"STATIC", L"Note", 0, 203); app.note = control(window, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, ID_NOTE);
        SendMessageW(app.trial, EM_SETLIMITTEXT, 40, 0); SendMessageW(app.order, EM_SETLIMITTEXT, 40, 0); SendMessageW(app.note, EM_SETLIMITTEXT, 240, 0);
        control(window, L"STATIC", L"Timer starts only on your click and is cleared when evidence changes. Without Start, time is unmeasured.\r\nHigh use/background/new/missing does not establish cause, harm or necessity. No AI, upload or automatic action.", 0, 204);
        app.last_tick = GetTickCount64(); SetTimer(window, 1, 100, NULL); show_selected(); return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = 1080; ((MINMAXINFO *)lp)->ptMinTrackSize.y = 760; return 0;
    case WM_READY: consume(); return 0;
    case WM_NOTIFY:
        if (!app.updating && ((NMHDR *)lp)->idFrom == ID_LIST && ((NMHDR *)lp)->code == LVN_ITEMCHANGED) show_selected();
        return 0;
    case WM_COMMAND:
        if (app.stopping) return 0;
        if ((LOWORD(wp) == ID_VIEW || LOWORD(wp) == ID_METHOD) && HIWORD(wp) == CBN_SELCHANGE) {
            app.selected_decision = app.selected_episode = 0; cancel_timer(); clear_response(); render();
        } else if (LOWORD(wp) == ID_REFRESH || LOWORD(wp) == ID_PREVIOUS || LOWORD(wp) == ID_NEXT) {
            Command c; memset(&c, 0, sizeof(c)); c.kind = COMMAND_REFRESH; c.offset = app.offset;
            if (LOWORD(wp) == ID_PREVIOUS) c.offset = app.offset >= PAGE_SIZE ? app.offset - PAGE_SIZE : 0;
            if (LOWORD(wp) == ID_NEXT && c.offset <= UINT_MAX - PAGE_SIZE) c.offset += PAGE_SIZE;
            (void)queue(&c);
        } else if (LOWORD(wp) == ID_START) {
            if (app.selected_decision && app.frequency.QuadPart > 0 && QueryPerformanceCounter(&app.started)) {
                app.timed_decision = app.selected_decision; SetWindowTextW(app.start, L"Timer running...");
                text(app.status, "Explicit task timer started for this evidence. Saving records elapsed UI task time; it does not measure improvement.");
            } else text(app.status, "Select evidence before starting the task timer.");
        } else if (LOWORD(wp) == ID_SAVE) save_evaluation();
        else if (LOWORD(wp) == ID_EXPORT && app.export_path[0]) {
            Command c; memset(&c, 0, sizeof(c)); c.kind = COMMAND_EXPORT; (void)queue(&c);
        }
        return 0;
    case WM_TIMER: {
        uint64_t now = GetTickCount64(), gap = now - app.last_tick;
        app.last_tick = now; ++app.heartbeat; if (gap > app.max_gap) app.max_gap = gap;
        consume();
        if (app.worker && WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) {
            if (app.stopping) DestroyWindow(window);
            else if (app.worker_error) EnableWindow(GetDlgItem(window, ID_SAVE), FALSE);
        }
        return 0;
    }
    case WM_CLOSE:
        if (!app.stopping) {
            app.stopping = 1; app.close_tick = GetTickCount64(); cancel_timer();
            text(app.status, "Closing: finishing queued local actions and releasing worker-owned history...");
            SetEvent(app.stop); SetEvent(app.wake);
            if (!app.worker || WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
int wmain(int argc, wchar_t **argv)
{
    INITCOMMONCONTROLSEX common;
    WNDCLASSW wc;
    MSG message;
    int i, result = 0;
    memset(&app, 0, sizeof(app));
    for (i = 1; i < argc; ++i) {
        char *target;
        if (i + 1 >= argc) goto usage;
        if (!wcscmp(argv[i], L"--db")) target = app.db;
        else if (!wcscmp(argv[i], L"--export")) target = app.export_path;
        else goto usage;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[++i], -1, target, 32768, NULL, NULL)) goto usage;
    }
    if (!app.db[0]) goto usage;
    InitializeSRWLock(&app.lock); (void)QueryPerformanceFrequency(&app.frequency);
    app.stop = CreateEventW(NULL, TRUE, FALSE, NULL); app.wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app.stop || !app.wake) return 1;
    common.dwSize = sizeof(common); common.dwICC = ICC_LISTVIEW_CLASSES; InitCommonControlsEx(&common);
    memset(&wc, 0, sizeof(wc)); wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"LocalChangeExperimentV1"; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc);
    if (!CreateWindowW(wc.lpszClassName, L"Local change-event prototype", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 1340, 960, NULL, NULL, wc.hInstance, NULL)) return 1;
    app.worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!app.worker) { DestroyWindow(app.window); result = -1; }
    else while ((result = (int)GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(app.window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    SetEvent(app.stop); SetEvent(app.wake);
    if (app.worker) { WaitForSingleObject(app.worker, INFINITE); CloseHandle(app.worker); }
    free_view(app.latest); free_view(app.display); CloseHandle(app.stop); CloseHandle(app.wake);
    printf("prototype_gui ui_heartbeat=%llu max_ui_gap_ms=%llu graceful_close_ms=%llu\n",
        (unsigned long long)app.heartbeat, (unsigned long long)app.max_gap,
        (unsigned long long)(app.close_tick ? GetTickCount64() - app.close_tick : 0));
    return result < 0 || app.worker_error ? 1 : 0;
usage:
    fputs("prototype_gui --db EXISTING_EVENTS.sqlite [--export NEW.csv]\nCreate a demo or live prototype pair with prototype_cli first.\n", stderr); return 2;
}
