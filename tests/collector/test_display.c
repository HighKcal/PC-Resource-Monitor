#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "resource_display.h"

/* Exercise the production collector with controlled Win32 observations. The
 * shared resource_core is linked unchanged. No real process is modified. */
typedef struct FakeProcess {
    DWORD pid, open_error, times_error, memory_error, io_error, name_error;
    DWORD exit_error, exit_code;
    uint64_t creation;
    const WCHAR *list_name, *image_path;
} FakeProcess;
static FakeProcess processes[260];
static size_t process_count, enumeration_index;
static unsigned checks, failures, handles, allocations, fail_allocation;
static size_t live_allocations;
static DWORD requested_access;
static int enumeration_failure;
static LONGLONG ticks = 1000;
static uint64_t utc = UINT64_C(1000000000);

#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); } } while (0)

static int fail_alloc(void) { return ++allocations == fail_allocation; }
static void *fake_malloc(size_t n)
{
    void *p;
    if (fail_alloc()) return NULL;
    p = malloc(n); if (p) ++live_allocations; return p;
}
static void *fake_calloc(size_t n, size_t size)
{
    void *p;
    if (fail_alloc()) return NULL;
    p = calloc(n, size); if (p) ++live_allocations; return p;
}
static void *fake_realloc(void *old, size_t n)
{
    void *p;
    if (fail_alloc()) return NULL;
    p = realloc(old, n); if (p && !old) ++live_allocations; return p;
}
static void fake_free(void *p)
{
    if (p) { CHECK(live_allocations > 0); --live_allocations; }
    free(p);
}
static FILETIME fake_time(uint64_t n)
{
    FILETIME t; t.dwLowDateTime = (DWORD)n; t.dwHighDateTime = (DWORD)(n >> 32); return t;
}
static void WINAPI fake_utc(FILETIME *out) { *out = fake_time(++utc); }
static BOOL WINAPI fake_qpc(LARGE_INTEGER *out) { out->QuadPart = ++ticks; return TRUE; }
static BOOL WINAPI fake_frequency(LARGE_INTEGER *out) { out->QuadPart = 1000; return TRUE; }
static DWORD WINAPI fake_cpus(WORD group) { (void)group; return 8; }
static HWND WINAPI fake_foreground(void) { return NULL; }
static HANDLE WINAPI fake_snapshot(DWORD flags, DWORD pid)
{
    CHECK(flags == TH32CS_SNAPPROCESS && pid == 0); ++handles;
    return (HANDLE)(uintptr_t)1;
}
static BOOL fake_entry(PROCESSENTRY32W *entry)
{
    if (enumeration_failure && enumeration_index == 1) {
        SetLastError(ERROR_BAD_LENGTH); return FALSE;
    }
    if (enumeration_index >= process_count) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    CHECK(entry->dwSize == sizeof(*entry));
    entry->th32ProcessID = processes[enumeration_index].pid;
    wcsncpy_s(entry->szExeFile, MAX_PATH, processes[enumeration_index].list_name, _TRUNCATE);
    return TRUE;
}
static BOOL WINAPI fake_first(HANDLE h, PROCESSENTRY32W *entry)
{
    CHECK(h == (HANDLE)(uintptr_t)1); enumeration_index = 0; return fake_entry(entry);
}
static BOOL WINAPI fake_next(HANDLE h, PROCESSENTRY32W *entry)
{
    CHECK(h == (HANDLE)(uintptr_t)1); ++enumeration_index; return fake_entry(entry);
}
static FakeProcess *fake_process(HANDLE h)
{
    size_t index = (size_t)(uintptr_t)h - 64;
    CHECK(index < process_count); return &processes[index];
}
static HANDLE WINAPI fake_open(DWORD access, BOOL inherit, DWORD pid)
{
    size_t i;
    requested_access = access;
    CHECK(access == PROCESS_QUERY_LIMITED_INFORMATION && !inherit);
    for (i = 0; i < process_count; ++i) if (processes[i].pid == pid) break;
    CHECK(i < process_count);
    if (processes[i].open_error) { SetLastError(processes[i].open_error); return NULL; }
    ++handles; return (HANDLE)(uintptr_t)(64 + i);
}
static BOOL WINAPI fake_close(HANDLE h)
{
    (void)h; CHECK(handles > 0); --handles; return TRUE;
}
static BOOL WINAPI fake_times(HANDLE h, FILETIME *creation, FILETIME *exit_time,
    FILETIME *kernel, FILETIME *user)
{
    FakeProcess *p = fake_process(h);
    if (p->times_error) { SetLastError(p->times_error); return FALSE; }
    *creation = fake_time(p->creation); *exit_time = fake_time(0);
    *kernel = fake_time((uint64_t)ticks * 100); *user = fake_time((uint64_t)ticks * 50);
    return TRUE;
}
static BOOL WINAPI fake_memory(HANDLE h, PPROCESS_MEMORY_COUNTERS out, DWORD size)
{
    FakeProcess *p = fake_process(h); CHECK(size == sizeof(*out));
    if (p->memory_error) { SetLastError(p->memory_error); return FALSE; }
    out->WorkingSetSize = 123456; return TRUE;
}
static BOOL WINAPI fake_io(HANDLE h, PIO_COUNTERS out)
{
    FakeProcess *p = fake_process(h);
    if (p->io_error) { SetLastError(p->io_error); return FALSE; }
    memset(out, 0, sizeof(*out));
    out->ReadTransferCount = (uint64_t)ticks * 2;
    out->WriteTransferCount = (uint64_t)ticks * 3; return TRUE;
}
static BOOL WINAPI fake_name(HANDLE h, DWORD flags, LPWSTR out, PDWORD length)
{
    FakeProcess *p = fake_process(h); size_t n = wcslen(p->image_path);
    CHECK(flags == 0);
    if (p->name_error) { SetLastError(p->name_error); return FALSE; }
    CHECK(n < *length); wcscpy_s(out, *length, p->image_path); *length = (DWORD)n; return TRUE;
}
static BOOL WINAPI fake_exit(HANDLE h, LPDWORD code)
{
    FakeProcess *p = fake_process(h);
    if (p->exit_error) { SetLastError(p->exit_error); return FALSE; }
    *code = p->exit_code; return TRUE;
}

#define malloc fake_malloc
#define calloc fake_calloc
#define realloc fake_realloc
#define free fake_free
#define GetSystemTimeAsFileTime fake_utc
#define QueryPerformanceCounter fake_qpc
#define QueryPerformanceFrequency fake_frequency
#define GetActiveProcessorCount fake_cpus
#define GetForegroundWindow fake_foreground
#define CreateToolhelp32Snapshot fake_snapshot
#define Process32FirstW fake_first
#define Process32NextW fake_next
#define OpenProcess fake_open
#define CloseHandle fake_close
#define GetProcessTimes fake_times
#undef GetProcessMemoryInfo
#define GetProcessMemoryInfo fake_memory
#define GetProcessIoCounters fake_io
#define QueryFullProcessImageNameW fake_name
#define GetExitCodeProcess fake_exit
#include "../../collector/resource_collector.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

static void configure(size_t count)
{
    size_t i;
    memset(processes, 0, sizeof(processes)); process_count = count;
    enumeration_failure = 0; fail_allocation = 0;
    for (i = 0; i < count; ++i) {
        processes[i].pid = (DWORD)(100 + i); processes[i].creation = 100000;
        processes[i].list_name = L"listed.exe"; processes[i].image_path = L"C:\\apps\\handle.exe";
        processes[i].exit_code = STILL_ACTIVE;
    }
}
static void free_pair(ResourceBatch *raw, ResourceDisplayBatch *display)
{
    resource_batch_free(raw); resource_display_batch_free(display);
    CHECK(raw->items == NULL && raw->count == 0);
    CHECK(display->items == NULL && display->count == 0);
    CHECK(handles == 0);
}
static void collect(ResourceCollector *collector, ResourceBatch *raw, ResourceDisplayBatch *display)
{
    uint32_t error = 123;
    ticks += 1000;
    CHECK(resource_collector_collect_display(collector, raw, display, &error) == RESOURCE_OK);
    CHECK(error == 0 && raw->count == process_count && display->count == process_count);
    CHECK(handles == 0);
}
static void test_sources_and_failure(void)
{
    ResourceCollector *collector = NULL;
    ResourceBatch raw = {0}; ResourceDisplayBatch display = {0}; uint32_t error;
    size_t i;
    configure(6);
    processes[1].open_error = ERROR_ACCESS_DENIED;
    processes[2].open_error = ERROR_ACCESS_DENIED;
    processes[3].pid = 0; processes[3].open_error = ERROR_INVALID_PARAMETER;
    processes[4].pid = 4; processes[4].open_error = ERROR_ACCESS_DENIED;
    processes[5].name_error = ERROR_ACCESS_DENIED;
    CHECK(resource_collector_create(NULL, &collector, &error) == RESOURCE_OK);
    collect(collector, &raw, &display);
    CHECK(requested_access == PROCESS_QUERY_LIMITED_INFORMATION);
    CHECK(display.items[0].name_source == RESOURCE_NAME_HANDLE);
    CHECK(display.items[0].snapshot_name_utf8 == NULL);
    CHECK(strcmp(raw.items[0].process_name_utf8, "handle.exe") == 0);
    CHECK((raw.items[0].valid_fields & RESOURCE_VALID_NAME) != 0);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_FIRST_SAMPLE);
    for (i = 1; i < 5; ++i) {
        CHECK(display.items[i].name_source == RESOURCE_NAME_LIST);
        CHECK(strcmp(display.items[i].snapshot_name_utf8, "listed.exe") == 0);
        CHECK(raw.items[i].process_name_utf8 == NULL);
        CHECK((raw.items[i].valid_fields & (RESOURCE_VALID_NAME | RESOURCE_VALID_IDENTITY |
            RESOURCE_VALID_CPU | RESOURCE_VALID_MEMORY | RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE)) == 0);
        CHECK(display.items[i].attempted_fields == 0);
        CHECK(raw.items[i].times_error == 0 && raw.items[i].memory_error == 0 &&
            raw.items[i].io_error == 0 && raw.items[i].name_error == 0);
        CHECK(!display.items[i].exit_observed && display.items[i].exit_query_error == 0);
    }
    CHECK(display.items[1].snapshot_name_utf8 != display.items[2].snapshot_name_utf8);
    CHECK(display.items[3].special_process == RESOURCE_SPECIAL_IDLE);
    CHECK(display.items[4].special_process == RESOURCE_SPECIAL_SYSTEM);
    CHECK(display.items[1].special_process == RESOURCE_SPECIAL_NONE);
    CHECK(display.items[5].name_source == RESOURCE_NAME_LIST);
    CHECK((raw.items[5].valid_fields & RESOURCE_VALID_IDENTITY) != 0);
    CHECK((raw.items[5].valid_fields & RESOURCE_VALID_MEMORY) != 0);
    CHECK(raw.items[5].name_error == ERROR_ACCESS_DENIED);
    CHECK(display.items[5].attempted_fields == (RESOURCE_QUERY_TIMES | RESOURCE_QUERY_MEMORY |
        RESOURCE_QUERY_IO | RESOURCE_QUERY_NAME | RESOURCE_QUERY_EXIT));
    CHECK(!display.items[5].exit_observed && display.items[5].exit_code == STILL_ACTIVE);
    for (i = 0; i < raw.count; ++i) {
        CHECK(display.items[i].pid == raw.items[i].pid);
        CHECK(display.items[i].creation_time_utc_100ns == raw.items[i].creation_time_utc_100ns);
        CHECK(display.items[i].measured_at_utc_100ns == raw.items[i].measured_at_utc_100ns);
    }
    CHECK(resource_collector_collect_display(collector, &raw, &display, &error) == RESOURCE_INVALID_ARGUMENT);
    CHECK(strcmp(display.items[1].snapshot_name_utf8, "listed.exe") == 0);
    resource_batch_free(&raw);
    CHECK(strcmp(display.items[1].snapshot_name_utf8, "listed.exe") == 0);
    CHECK(resource_collector_collect_display(collector, &raw, &display, &error) == RESOURCE_INVALID_ARGUMENT);
    CHECK(raw.items == NULL);
    resource_display_batch_free(&display);
    processes[5].list_name = L"new-list.exe";
    collect(collector, &raw, &display);
    CHECK(raw.items[5].cpu_status == RESOURCE_DELTA_VALID && raw.items[5].io_read_status == RESOURCE_DELTA_VALID);
    CHECK(strcmp(display.items[5].snapshot_name_utf8, "new-list.exe") == 0);
    resource_collector_destroy(collector);
    CHECK(strcmp(display.items[5].snapshot_name_utf8, "new-list.exe") == 0);
    free_pair(&raw, &display);
    resource_display_batch_free(&display);
    CHECK(live_allocations == 0);
}
static void test_partial_identity_and_exit(void)
{
    ResourceCollector *collector = NULL; ResourceBatch raw = {0};
    ResourceDisplayBatch display = {0}; uint32_t error;
    configure(2);
    CHECK(resource_collector_create(NULL, &collector, &error) == RESOURCE_OK);
    collect(collector, &raw, &display); free_pair(&raw, &display);
    processes[0].memory_error = ERROR_ACCESS_DENIED;
    collect(collector, &raw, &display);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_VALID && raw.items[0].io_read_status == RESOURCE_DELTA_VALID);
    CHECK((raw.items[0].valid_fields & RESOURCE_VALID_MEMORY) == 0);
    CHECK((raw.items[0].valid_fields & RESOURCE_VALID_NAME) != 0);
    CHECK(!display.items[0].exit_observed);
    free_pair(&raw, &display);
    processes[0].memory_error = 0; processes[0].times_error = ERROR_ACCESS_DENIED;
    collect(collector, &raw, &display);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    CHECK(raw.items[0].io_read_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    CHECK((raw.items[0].valid_fields & RESOURCE_VALID_MEMORY) != 0);
    CHECK(display.items[0].creation_time_utc_100ns == 0);
    free_pair(&raw, &display);
    processes[0].times_error = 0;
    collect(collector, &raw, &display);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    free_pair(&raw, &display);
    ++processes[0].creation; processes[0].name_error = ERROR_PARTIAL_COPY;
    processes[0].list_name = L"old-instance.exe";
    processes[0].memory_error = processes[0].io_error = ERROR_PARTIAL_COPY;
    processes[0].exit_code = 0;
    collect(collector, &raw, &display);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_IDENTITY_CHANGED);
    CHECK(isnan(raw.items[0].cpu_percent) && isnan(raw.items[0].io_read_bytes_per_second));
    CHECK(display.items[0].exit_observed && display.items[0].exit_code == 0);
    CHECK(display.items[0].name_source == RESOURCE_NAME_LIST);
    CHECK(raw.items[0].process_name_utf8 == NULL);
    CHECK(raw.items[1].cpu_status == RESOURCE_DELTA_VALID);
    free_pair(&raw, &display);
    processes[0].open_error = ERROR_INVALID_PARAMETER;
    collect(collector, &raw, &display);
    CHECK(!display.items[0].exit_observed && display.items[0].attempted_fields == 0);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    free_pair(&raw, &display);
    processes[0].open_error = 0; processes[0].exit_error = ERROR_INVALID_HANDLE;
    collect(collector, &raw, &display);
    CHECK(display.items[0].exit_query_error == ERROR_INVALID_HANDLE && !display.items[0].exit_observed);
    free_pair(&raw, &display);
    resource_collector_destroy(collector); CHECK(live_allocations == 0);
}
static void test_unavailable_and_legacy(void)
{
    ResourceCollector *collector = NULL; ResourceBatch raw = {0};
    ResourceDisplayBatch display = {0}; uint32_t error;
    static const WCHAR invalid_utf16[] = { 0xd800, 0 };
    configure(2); processes[0].open_error = processes[1].open_error = ERROR_ACCESS_DENIED;
    processes[0].list_name = L""; processes[1].list_name = invalid_utf16;
    CHECK(resource_collector_create(NULL, &collector, &error) == RESOURCE_OK);
    collect(collector, &raw, &display);
    CHECK(display.items[0].name_source == RESOURCE_NAME_UNAVAILABLE);
    CHECK(display.items[1].name_source == RESOURCE_NAME_UNAVAILABLE);
    CHECK(display.items[1].snapshot_name_error == ERROR_NO_UNICODE_TRANSLATION);
    CHECK(display.items[1].snapshot_name_utf8 == NULL);
    free_pair(&raw, &display);
    configure(1); processes[0].name_error = ERROR_ACCESS_DENIED;
    CHECK(resource_collector_collect(collector, &raw, &error) == RESOURCE_OK);
    CHECK(raw.items[0].process_name_utf8 == NULL && !(raw.items[0].valid_fields & RESOURCE_VALID_NAME));
    resource_batch_free(&raw); resource_collector_destroy(collector);
    CHECK(live_allocations == 0 && handles == 0);
}
static void test_atomic_failures(void)
{
    ResourceCollector *collector = NULL; ResourceBatch raw = {0};
    ResourceDisplayBatch display = {0}; ResourceCollectorInfo info;
    uint32_t error; size_t i; unsigned fail_at; int reached_success = 0;
    configure(130);
    for (i = 0; i < process_count; ++i) if (i % 2) processes[i].open_error = ERROR_ACCESS_DENIED;
    CHECK(resource_collector_create(NULL, &collector, &error) == RESOURCE_OK);
    for (fail_at = 1; fail_at < 150; ++fail_at) {
        ResourceStatus status;
        allocations = 0; fail_allocation = fail_at;
        status = resource_collector_collect_display(collector, &raw, &display, &error);
        fail_allocation = 0;
        if (status == RESOURCE_OK) { reached_success = 1; free_pair(&raw, &display); break; }
        CHECK(status == RESOURCE_OUT_OF_MEMORY);
        CHECK(raw.items == NULL && raw.count == 0 && display.items == NULL && display.count == 0);
        resource_collector_get_info(collector, &info);
        CHECK(info.tracked_processes == 0 && handles == 0 && live_allocations == 1);
    }
    CHECK(reached_success);
    collect(collector, &raw, &display); free_pair(&raw, &display);
    enumeration_failure = 1;
    CHECK(resource_collector_collect_display(collector, &raw, &display, &error) == RESOURCE_PLATFORM_ERROR);
    CHECK(error == ERROR_BAD_LENGTH);
    CHECK(raw.items == NULL && display.items == NULL);
    resource_collector_get_info(collector, &info); CHECK(info.tracked_processes == 0);
    enumeration_failure = 0;
    collect(collector, &raw, &display);
    CHECK(raw.items[0].cpu_status == RESOURCE_DELTA_FIRST_SAMPLE);
    free_pair(&raw, &display); resource_collector_destroy(collector);
    CHECK(live_allocations == 0 && handles == 0);
}
int main(void)
{
    test_sources_and_failure(); test_partial_identity_and_exit();
    test_unavailable_and_legacy(); test_atomic_failures();
    printf("Collector display: %u checks, %u failures (API failures, snapshot provenance, identity, exit, OOM)\n", checks, failures);
    return failures ? 1 : 0;
}
