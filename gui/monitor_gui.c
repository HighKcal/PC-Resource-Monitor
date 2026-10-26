#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "monitor.h"

#define WM_SNAPSHOT (WM_APP + 1)
#define WM_WORKER_DONE (WM_APP + 2)
#define ID_LIST 101
#define ID_IGNORE 102
#define ID_EXPECTED 103
#define ID_CANCEL 104
#define ID_PAUSE 105
typedef struct FeedbackCommand { int64_t program; MonitorFeedback kind; uint64_t expires; } FeedbackCommand;
typedef struct App {
    HWND window, list, detail, status, pause;
    HANDLE stop, wake, worker;
    SRWLOCK lock;
    MonitorSnapshot *latest, *display;
    FeedbackCommand commands[16];
    FeedbackCommand feedback_results[16];
    unsigned feedback_result_count;
    FeedbackCommand feedback_overlay[16];
    unsigned feedback_overlay_count;
    unsigned command_count, count, interval_ms, completed;
    int headless, synthetic, pause_requested, stopping, worker_error, updating, stats;
    char db[32768];
    uint64_t heartbeat, last_tick, max_heartbeat_gap, stop_started;
} App;
static App app;

static const char *utc_text(uint64_t value, char *buffer, size_t size)
{
    FILETIME ft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)value; ft.dwHighDateTime = (DWORD)(value >> 32);
    if (!value || !FileTimeToSystemTime(&ft, &st)) snprintf(buffer, size, "not available");
    else snprintf(buffer, size, "%04u-%02u-%02u %02u:%02u:%02u.%03u UTC",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}
static BOOL WINAPI console_handler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        SetEvent(app.stop); return TRUE;
    }
    return FALSE;
}

static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n ? malloc((size_t)n * sizeof(*w)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static void set_text(HWND window, const char *s)
{
    wchar_t *w = to_wide(s); if (w) { SetWindowTextW(window, w); free(w); }
}
static void publish(MonitorSnapshot *s)
{
    MonitorSnapshot *old;
    AcquireSRWLockExclusive(&app.lock); old = app.latest; app.latest = s;
    ReleaseSRWLockExclusive(&app.lock);
    monitor_snapshot_free(old);
    if (app.window) PostMessageW(app.window, WM_SNAPSHOT, 0, 0);
}
static MonitorSnapshot *error_snapshot(const char *message, int store)
{
    MonitorSnapshot *s = calloc(1, sizeof(*s));
    if (s) {
        s->storage_error = store; s->collector_error = !store; s->synthetic = app.synthetic;
        s->measured_utc_100ns = monitor_utc_now(); snprintf(s->status, sizeof(s->status), "%s", message);
    }
    return s;
}
static void synthetic_batch(ResourceBatch *batch, ResourceData *row, uint64_t now, unsigned cycle)
{
    memset(row, 0, sizeof(*row));
    row->pid = 4242; row->creation_time_utc_100ns = now - (uint64_t)cycle * 3 * UINT64_C(10000000) - UINT64_C(10000000);
    row->process_name_utf8 = "SYNTHETIC fixture.exe";
    row->measured_at_utc_100ns = now;
    row->valid_fields = RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY | RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY |
        RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE | RESOURCE_VALID_OBSERVATION_INTERVAL | RESOURCE_VALID_FOREGROUND;
    row->cpu_percent = cycle > 60 ? 50 : 1; row->working_set_bytes = UINT64_C(67108864);
    row->observation_interval_seconds = row->cpu_interval_seconds = row->io_interval_seconds = 3;
    row->foreground = RESOURCE_BACKGROUND;
    memset(batch, 0, sizeof(*batch)); batch->items = row; batch->count = 1;
    batch->started_at_utc_100ns = batch->finished_at_utc_100ns = now; batch->logical_processor_count = 1;
}
static DWORD WINAPI worker_main(void *unused)
{
    MonitorEngine *engine = NULL;
    ResourceCollector *collector = NULL;
    char error[256];
    uint32_t os_error = 0;
    uint64_t synthetic_start = monitor_utc_now();
    int was_paused = 0;
    (void)unused;
    if (!monitor_open(app.db, app.synthetic, &engine, error, sizeof(error))) {
        publish(error_snapshot(error, 1)); app.worker_error = 1; goto finished;
    }
    printf("SQLite=%s mode=%s raw_retention=1h summary_retention=28d DB=%s\n", monitor_storage_version(),
        app.synthetic ? "SYNTHETIC" : "real", app.db); fflush(stdout);
    if (!app.synthetic && resource_collector_create(NULL, &collector, &os_error) != RESOURCE_OK) {
        snprintf(error, sizeof(error), "Collector initialization failed: Win32 %lu", (unsigned long)os_error);
        publish(error_snapshot(error, 0)); app.worker_error = 1; goto finished;
    }
    while (WaitForSingleObject(app.stop, 0) != WAIT_OBJECT_0) {
        ResourceBatch batch = {0};
        ResourceData fixture;
        MonitorSnapshot *snapshot = NULL;
        LARGE_INTEGER begin, end, freq;
        FeedbackCommand commands[16];
        unsigned n, i;
        int paused;
        AcquireSRWLockExclusive(&app.lock);
        n = app.command_count; memcpy(commands, app.commands, n * sizeof(*commands)); app.command_count = 0;
        paused = app.pause_requested; ResetEvent(app.wake);
        ReleaseSRWLockExclusive(&app.lock);
        for (i = 0; i < n; ++i) {
            uint64_t feedback_now = monitor_utc_now();
            if (!monitor_feedback(engine, commands[i].program, commands[i].kind, feedback_now)) {
                publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished;
            }
            commands[i].expires = commands[i].kind ? feedback_now +
                (commands[i].kind == MONITOR_FEEDBACK_IGNORE ? UINT64_C(36000000000) : UINT64_C(864000000000)) : 0;
            AcquireSRWLockExclusive(&app.lock);
            if (app.feedback_result_count < 16) app.feedback_results[app.feedback_result_count++] = commands[i];
            ReleaseSRWLockExclusive(&app.lock);
        }
        if (paused) {
            HANDLE waits[2] = { app.stop, app.wake };
            if (!was_paused) {
                monitor_reset(engine); resource_collector_destroy(collector); collector = NULL; was_paused = 1;
            }
            WaitForMultipleObjects(2, waits, FALSE, 250); continue;
        }
        if (was_paused && !app.synthetic) {
            if (resource_collector_create(NULL, &collector, &os_error) != RESOURCE_OK) {
                publish(error_snapshot("Collector resume failed", 0)); app.worker_error = 1; goto finished;
            }
        }
        was_paused = 0;
        QueryPerformanceCounter(&begin); QueryPerformanceFrequency(&freq);
        if (app.synthetic) synthetic_batch(&batch, &fixture,
            synthetic_start + (uint64_t)app.completed * 3 * UINT64_C(10000000), app.completed);
        else if (resource_collector_collect(collector, &batch, &os_error) != RESOURCE_OK) {
            if (!monitor_record_failure(engine, monitor_utc_now(), os_error)) {
                publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished;
            }
            snprintf(error, sizeof(error), "Collector failed: Win32 %lu; windows reset", (unsigned long)os_error);
            publish(error_snapshot(error, 0)); app.worker_error = 1; goto finished;
        }
        QueryPerformanceCounter(&end);
        {
            const char *paths[1] = { "SYNTHETIC:\\fixture.exe" };
            int ok = monitor_ingest(engine, &batch, app.synthetic ? paths : NULL, &snapshot);
            if (!app.synthetic) resource_batch_free(&batch);
            if (!ok) { publish(error_snapshot(monitor_error(engine), 1)); app.worker_error = 1; goto finished; }
        }
        snapshot->collect_ms = (double)(end.QuadPart - begin.QuadPart) * 1000 / (double)freq.QuadPart;
        ++app.completed;
        printf("batch=%u rows=%zu windows=%zu collect_ms=%.3f store_ms=%.3f %s\n", app.completed,
            snapshot->count, snapshot->completed_windows, snapshot->collect_ms, snapshot->store_ms, snapshot->status);
        fflush(stdout); publish(snapshot);
        if (app.count && app.completed >= app.count) break;
        {
            HANDLE waits[2] = { app.stop, app.wake };
            WaitForMultipleObjects(2, waits, FALSE, app.interval_ms);
        }
    }
finished:
    if (engine && app.stats) {
        MonitorStorageStats stats;
        if (monitor_storage_stats(engine, &stats)) printf(
            "stats raw_rows=%llu windows=%llu metric_rows=%llu page_bytes=%llu raw_page_bytes=%llu aggregate_page_bytes=%llu raw_bytes_per_row=%.3f window_bytes_including_metrics=%.3f\n",
            (unsigned long long)stats.raw_rows, (unsigned long long)stats.windows, (unsigned long long)stats.metric_rows,
            (unsigned long long)stats.database_page_bytes, (unsigned long long)stats.raw_page_bytes,
            (unsigned long long)stats.aggregate_page_bytes, stats.raw_rows ? (double)stats.raw_page_bytes / (double)stats.raw_rows : 0,
            stats.windows ? (double)stats.aggregate_page_bytes / (double)stats.windows : 0);
        else fprintf(stderr, "stats unavailable: %s\n", monitor_error(engine));
    }
    resource_collector_destroy(collector); monitor_close(engine);
    if (app.window) PostMessageW(app.window, WM_WORKER_DONE, 0, 0);
    return 0;
}
static void list_text(int row, int column, const char *text_value)
{
    wchar_t *w = to_wide(text_value);
    if (w) { ListView_SetItemText(app.list, row, column, w); free(w); }
}
static const char *number(char *s, size_t n, int valid, double v, const char *format)
{
    if (valid && isfinite(v)) snprintf(s, n, format, v); else snprintf(s, n, "NA"); return s;
}
static int selected_row(void) { return ListView_GetNextItem(app.list, -1, LVNI_SELECTED); }
static MonitorFeedback displayed_feedback(const MonitorRow *row, uint64_t *expires)
{
    unsigned i;
    *expires = row->feedback_expires_utc_100ns;
    for (i = 0; i < app.feedback_overlay_count; ++i) if (app.feedback_overlay[i].program == row->program_id) {
        *expires = app.feedback_overlay[i].expires;
        return app.feedback_overlay[i].expires > monitor_utc_now() ? app.feedback_overlay[i].kind : MONITOR_FEEDBACK_CANCEL;
    }
    return *expires > monitor_utc_now() ? row->feedback : MONITOR_FEEDBACK_CANCEL;
}
static void show_detail(void)
{
    int index = selected_row();
    char text[8192], line[768], observed[64], created[64], expires[64], foreground[64];
    MonitorRow *row;
    unsigned m;
    size_t used;
    uint64_t feedback_expires;
    MonitorFeedback feedback;
    if (index < 0 || !app.display || (size_t)index >= app.display->count) {
        set_text(app.detail, "Select a process for current observation and last completed window evidence.\r\n"
            "Baseline: same exact executable path, individual process windows; app totals are not measured.\r\n"
            "Raw 1h / summaries 28d. Foreground is a momentary observation. Missing values are NA.\r\n"
            "High resource use, new programs, and background status do not establish harmful or unnecessary activity."); return;
    }
    row = &app.display->rows[index];
    feedback = displayed_feedback(row, &feedback_expires);
    snprintf(text, sizeof(text), "%s PID %lu created %s\r\nPath: %s\r\n"
        "Current observation %s valid=0x%lx foreground=%d (momentary)\r\n"
        "Foreground observed %s; snapshot PID=%lu\r\n"
        "Win32 open/times/memory/io/name/path errors: %lu / %lu / %lu / %lu / %lu / %lu\r\n"
        "CPU/read/write delta: %s / %s / %s\r\n"
        "Below: LAST COMPLETED 60-75s INSTANCE WINDOW, not the current raw row.\r\n",
        row->name, (unsigned long)row->raw.pid, utc_text(row->raw.creation_time_utc_100ns, created, sizeof(created)),
        row->path ? row->path : "unavailable; excluded from learning", utc_text(row->raw.measured_at_utc_100ns, observed, sizeof(observed)),
        (unsigned long)row->raw.valid_fields, (int)row->raw.foreground,
        utc_text(app.display->foreground_observed_utc_100ns, foreground, sizeof(foreground)), (unsigned long)app.display->foreground_pid,
        (unsigned long)row->raw.open_error, (unsigned long)row->raw.times_error, (unsigned long)row->raw.memory_error,
        (unsigned long)row->raw.io_error, (unsigned long)row->raw.name_error, (unsigned long)row->path_error,
        resource_delta_status_string(row->raw.cpu_status), resource_delta_status_string(row->raw.io_read_status), resource_delta_status_string(row->raw.io_write_status));
    for (m = 0; m < 4; ++m) {
        MonitorMetricReport *r = &row->metric[m];
        char cur[64], med[64], mad[64], threshold[64];
        char start[64], end[64];
        snprintf(line, sizeof(line), "%s: %s; value=%s median=%s MAD=%s T=%s\r\n"
            "  past n=%u UTC dates=%u distinct minutes=%u; coverage=%.3fs valid count=%u streak=%u\r\n"
            "  window %s to %s; duration=%.3fs\r\n"
            "  %s; baseline eligible=%d\r\n", monitor_metric_name(m), monitor_state_name(r->state),
            number(cur, sizeof(cur), isfinite(r->current), r->current, "%.3f"),
            number(med, sizeof(med), isfinite(r->median), r->median, "%.3f"),
            number(mad, sizeof(mad), isfinite(r->mad), r->mad, "%.3f"),
            number(threshold, sizeof(threshold), isfinite(r->threshold), r->threshold, "%.3f"),
            r->count, r->days, r->minutes, r->covered_seconds, r->valid_samples, r->streak,
            utc_text(r->window_start_utc_100ns, start, sizeof(start)), utc_text(r->window_end_utc_100ns, end, sizeof(end)),
            r->window_duration_seconds, r->reason, r->baseline_eligible);
        used = strlen(text); snprintf(text + used, sizeof(text) - used, "%s", line);
    }
    used = strlen(text);
    snprintf(text + used, sizeof(text) - used,
        "Feedback: %s; expires %s. Candidate facts and baseline are unchanged.\r\n"
        "T=median+max(3*1.4826*MAD,0.5*median,floor). Floors CPU 5pp, RAM 128MiB, I/O 1MiB/s.\r\n"
        "Past-only; latest 64/day in current+13 prior UTC dates. Gates 60 windows/3 dates/60 minutes.\r\n"
        "Feedback suppresses this exact path only: Ignore 1h / Expected 24h. No automatic process termination.",
        feedback == MONITOR_FEEDBACK_IGNORE ? "Ignore 1h (suppressed)" :
        feedback == MONITOR_FEEDBACK_EXPECTED ? "Expected 24h (suppressed)" : "none/expired",
        utc_text(feedback_expires, expires, sizeof(expires)));
    set_text(app.detail, text);
}
static void consume_snapshot(void)
{
    MonitorSnapshot *s;
    uint32_t selected_pid = 0;
    uint64_t selected_creation = 0;
    uint32_t top_pid = 0;
    uint64_t top_creation = 0;
    int selection = selected_row();
    int top = ListView_GetTopIndex(app.list), restored_top = -1;
    size_t i;
    char value[96], status[768];
    if (app.display && selection >= 0 && (size_t)selection < app.display->count) {
        selected_pid = app.display->rows[selection].raw.pid;
        selected_creation = app.display->rows[selection].raw.creation_time_utc_100ns;
    }
    if (app.display && top >= 0 && (size_t)top < app.display->count) {
        top_pid = app.display->rows[top].raw.pid; top_creation = app.display->rows[top].raw.creation_time_utc_100ns;
    }
    AcquireSRWLockExclusive(&app.lock); s = app.latest; app.latest = NULL; ReleaseSRWLockExclusive(&app.lock);
    if (!s) return;
    app.updating = 1; SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(app.list); monitor_snapshot_free(app.display); app.display = s;
    for (i = 0; i < s->count; ++i) {
        MonitorRow *row = &s->rows[i];
        LVITEMW item;
        unsigned m, ready = 0;
        MonitorState overall = MONITOR_UNAVAILABLE;
        uint64_t feedback_expires;
        memset(&item, 0, sizeof(item)); item.mask = LVIF_TEXT; item.iItem = (int)i;
        item.pszText = to_wide(row->name); ListView_InsertItem(app.list, &item); free(item.pszText);
        snprintf(value, sizeof(value), "%lu", (unsigned long)row->raw.pid); list_text((int)i, 1, value);
        list_text((int)i, 2, number(value, sizeof(value), row->raw.valid_fields & RESOURCE_VALID_CPU, row->raw.cpu_percent, "%.2f"));
        list_text((int)i, 3, number(value, sizeof(value), row->raw.valid_fields & RESOURCE_VALID_MEMORY, (double)row->raw.working_set_bytes / 1048576, "%.1f"));
        list_text((int)i, 4, number(value, sizeof(value), row->raw.valid_fields & RESOURCE_VALID_IO_READ, row->raw.io_read_bytes_per_second, "%.0f"));
        list_text((int)i, 5, number(value, sizeof(value), row->raw.valid_fields & RESOURCE_VALID_IO_WRITE, row->raw.io_write_bytes_per_second, "%.0f"));
        for (m = 0; m < 4; ++m) {
            if (row->metric[m].state > overall) overall = row->metric[m].state;
            if (row->metric[m].state >= MONITOR_NORMAL) ++ready;
        }
        snprintf(value, sizeof(value), "%s%s%s", monitor_state_name(overall), ready > 0 && ready < 4 ? " (partial)" : "",
            displayed_feedback(row, &feedback_expires) ? " (suppressed)" : ""); list_text((int)i, 6, value);
        list_text((int)i, 7, row->raw.foreground == RESOURCE_FOREGROUND ? "foreground" :
            row->raw.foreground == RESOURCE_BACKGROUND ? "background" : "unknown");
        if (row->raw.pid == selected_pid && row->raw.creation_time_utc_100ns == selected_creation)
            ListView_SetItemState(app.list, (int)i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        if (row->raw.pid == top_pid && row->raw.creation_time_utc_100ns == top_creation) restored_top = (int)i;
    }
    if (s->count) {
        RECT bounds;
        if (restored_top < 0) restored_top = top < (int)s->count ? top : (int)s->count - 1;
        if (ListView_GetItemRect(app.list, 0, &bounds, LVIR_BOUNDS))
            ListView_Scroll(app.list, 0, (restored_top - ListView_GetTopIndex(app.list)) * (bounds.bottom - bounds.top));
    }
    app.updating = 0;
    SendMessageW(app.list, WM_SETREDRAW, TRUE, 0); InvalidateRect(app.list, NULL, TRUE);
    snprintf(status, sizeof(status), "%s | collect %.1fms store %.1fms | %s", s->status, s->collect_ms, s->store_ms, app.db);
    set_text(app.status, status); show_detail();
}
static void layout(HWND window)
{
    RECT r;
    int width, height, list_height;
    GetClientRect(window, &r); width = r.right; height = r.bottom;
    list_height = (height - 100) / 2;
    MoveWindow(app.status, 10, 8, width - 20, 44, TRUE);
    MoveWindow(app.list, 10, 55, width - 20, list_height, TRUE);
    MoveWindow(app.detail, 10, list_height + 65, width - 20, height - list_height - 115, TRUE);
    MoveWindow(app.pause, 10, height - 42, 120, 30, TRUE);
    MoveWindow(GetDlgItem(window, ID_IGNORE), 140, height - 42, 140, 30, TRUE);
    MoveWindow(GetDlgItem(window, ID_EXPECTED), 290, height - 42, 160, 30, TRUE);
    MoveWindow(GetDlgItem(window, ID_CANCEL), 460, height - 42, 160, 30, TRUE);
}
static void begin_close(void)
{
    if (app.stopping) return;
    app.stopping = 1; app.stop_started = GetTickCount64();
    set_text(app.status, "Stopping: waiting for worker to finish and close local database...");
    SetEvent(app.stop); EnableWindow(app.pause, FALSE);
}
static void consume_feedback(void)
{
    FeedbackCommand results[16];
    unsigned i, n;
    AcquireSRWLockExclusive(&app.lock);
    n = app.feedback_result_count; memcpy(results, app.feedback_results, n * sizeof(*results)); app.feedback_result_count = 0;
    ReleaseSRWLockExclusive(&app.lock);
    if (!n || !app.display) return;
    for (i = 0; i < n; ++i) {
        unsigned j;
        for (j = 0; j < app.feedback_overlay_count && app.feedback_overlay[j].program != results[i].program; ++j) { }
        if (j == 16) j = 0;
        if (j == app.feedback_overlay_count) ++app.feedback_overlay_count;
        app.feedback_overlay[j] = results[i];
    }
    set_text(app.status, "Feedback saved locally; candidate evidence and baseline are unchanged"); show_detail();
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_CREATE: {
        const wchar_t *columns[] = { L"Process", L"PID", L"CPU % now", L"RAM MiB now", L"Read B/s now", L"Write B/s now", L"Last window", L"Foreground snapshot" };
        const int widths[] = { 220, 70, 90, 105, 110, 110, 180, 150 };
        unsigned i;
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        app.window = window;
        app.status = CreateWindowW(L"STATIC", L"Starting local worker...", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, NULL, NULL, NULL);
        app.list = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        for (i = 0; i < 8; ++i) {
            LVCOLUMNW c; memset(&c, 0, sizeof(c)); c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (wchar_t *)columns[i]; c.cx = widths[i]; ListView_InsertColumn(app.list, (int)i, &c);
        }
        app.detail = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | WS_BORDER,
            0, 0, 0, 0, window, NULL, NULL, NULL);
        app.pause = CreateWindowW(L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_PAUSE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Ignore 1 hour", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_IGNORE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Expected 24 hours", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_EXPECTED, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Cancel feedback", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_CANCEL, NULL, NULL);
        SendMessageW(app.status, WM_SETFONT, (WPARAM)font, TRUE); SendMessageW(app.list, WM_SETFONT, (WPARAM)font, TRUE);
        SendMessageW(app.detail, WM_SETFONT, (WPARAM)font, TRUE); show_detail();
        app.last_tick = GetTickCount64(); SetTimer(window, 1, 100, NULL);
        return 0;
    }
    case WM_SIZE: layout(window); return 0;
    case WM_SNAPSHOT: consume_snapshot(); return 0;
    case WM_NOTIFY:
        if (!app.updating && ((NMHDR *)lp)->idFrom == ID_LIST && ((NMHDR *)lp)->code == LVN_ITEMCHANGED) show_detail();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PAUSE && !app.stopping) {
            AcquireSRWLockExclusive(&app.lock); app.pause_requested = !app.pause_requested;
            ReleaseSRWLockExclusive(&app.lock); SetEvent(app.wake);
            SetWindowTextW(app.pause, app.pause_requested ? L"Resume" : L"Pause");
            set_text(app.status, app.pause_requested ? "Paused after current round; incomplete windows discarded" : "Resuming; new window anchors required");
        } else if (LOWORD(wp) >= ID_IGNORE && LOWORD(wp) <= ID_CANCEL && !app.stopping) {
            int selected = selected_row();
            if (selected >= 0 && app.display && (size_t)selected < app.display->count && app.display->rows[selected].program_id) {
                FeedbackCommand command;
                int queued = 0;
                command.program = app.display->rows[selected].program_id;
                command.kind = LOWORD(wp) == ID_IGNORE ? MONITOR_FEEDBACK_IGNORE :
                    LOWORD(wp) == ID_EXPECTED ? MONITOR_FEEDBACK_EXPECTED : MONITOR_FEEDBACK_CANCEL;
                command.expires = 0;
                AcquireSRWLockExclusive(&app.lock);
                if (app.command_count < 16) { app.commands[app.command_count++] = command; queued = 1; }
                ReleaseSRWLockExclusive(&app.lock); SetEvent(app.wake);
                set_text(app.status, queued ? "Feedback queued for worker; candidate evidence and learning gates remain unchanged" :
                    "Feedback queue is full; action was not saved. Wait for the worker and try again.");
            } else set_text(app.status, "Select a process with a verified executable path before feedback");
        }
        return 0;
    case WM_TIMER: {
        uint64_t now = GetTickCount64(), gap = now - app.last_tick;
        app.last_tick = now; ++app.heartbeat;
        if (gap > app.max_heartbeat_gap) app.max_heartbeat_gap = gap;
        consume_snapshot(); consume_feedback();
        if (!app.stopping && app.worker && WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) {
            if (app.count && app.completed >= app.count) begin_close();
            else if (app.worker_error) EnableWindow(app.pause, FALSE);
        }
        if (app.stopping && app.worker && WaitForSingleObject(app.worker, 0) == WAIT_OBJECT_0) DestroyWindow(window);
        else if (app.display && !app.pause_requested && !app.display->storage_error && !app.display->collector_error &&
            monitor_utc_now() > app.display->measured_utc_100ns + UINT64_C(150000000) && !app.synthetic)
            set_text(app.status, "STALE: no successful observation for over 15 seconds");
        return 0;
    }
    case WM_WORKER_DONE:
        consume_snapshot();
        if (app.count && app.completed >= app.count) begin_close();
        else if (app.worker_error) EnableWindow(app.pause, FALSE);
        return 0;
    case WM_CLOSE: begin_close(); return 0;
    case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
int wmain(int argc, wchar_t **argv)
{
    INITCOMMONCONTROLSEX common;
    WNDCLASSW wc;
    MSG msg;
    int i, result = 0;
    memset(&app, 0, sizeof(app)); app.interval_ms = 3000;
    snprintf(app.db, sizeof(app.db), "monitor-data.sqlite");
    for (i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--headless")) app.headless = 1;
        else if (!wcscmp(argv[i], L"--stats")) app.stats = 1;
        else if (!wcscmp(argv[i], L"--synthetic")) app.synthetic = 1;
        else if (!wcscmp(argv[i], L"--count") && i + 1 < argc) app.count = (unsigned)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--interval-ms") && i + 1 < argc) app.interval_ms = (unsigned)_wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--db") && i + 1 < argc) {
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[++i], -1, app.db, sizeof(app.db), NULL, NULL)) return 2;
        } else {
            fwprintf(stderr, L"Unrecognized or incomplete argument: %ls\n", argv[i]);
            printf("monitor_gui [--headless] [--stats] [--db LOCAL_PATH] [--count N] [--interval-ms N] [--synthetic]\n"); return 2;
        }
    }
    if (app.interval_ms < 1 || app.interval_ms > 60000 || (app.synthetic && !strcmp(app.db, "monitor-data.sqlite"))) {
        fprintf(stderr, "interval must be 1..60000; SYNTHETIC requires explicit separate --db\n"); return 2;
    }
    InitializeSRWLock(&app.lock); app.stop = CreateEventW(NULL, TRUE, FALSE, NULL); app.wake = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app.stop || !app.wake) return 1;
    SetConsoleCtrlHandler(console_handler, TRUE);
    if (!app.headless) {
        common.dwSize = sizeof(common); common.dwICC = ICC_LISTVIEW_CLASSES; InitCommonControlsEx(&common);
        memset(&wc, 0, sizeof(wc)); wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"LocalResourceMonitorV1"; wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); RegisterClassW(&wc);
        if (!CreateWindowW(wc.lpszClassName, app.synthetic ? L"SYNTHETIC - Local Resource Monitor" : L"Local Resource Monitor - learning from past observations",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1250, 900, NULL, NULL, wc.hInstance, NULL)) return 1;
    }
    app.worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    if (!app.worker) { if (app.window) DestroyWindow(app.window); return 1; }
    if (app.headless) WaitForSingleObject(app.worker, INFINITE);
    else while ((result = (int)GetMessageW(&msg, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(app.window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    SetEvent(app.stop); WaitForSingleObject(app.worker, INFINITE);
    printf("exit batches=%u ui_heartbeat=%llu max_ui_gap_ms=%llu graceful_stop_ms=%llu\n", app.completed,
        (unsigned long long)app.heartbeat, (unsigned long long)app.max_heartbeat_gap,
        (unsigned long long)(app.stop_started ? GetTickCount64() - app.stop_started : 0));
    if (app.headless && app.latest) printf("final_status=%s\n", app.latest->status);
    monitor_snapshot_free(app.latest); monitor_snapshot_free(app.display);
    CloseHandle(app.worker); CloseHandle(app.stop); CloseHandle(app.wake);
    return app.worker_error || result < 0 ? 1 : 0;
}
