#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include "resource_collector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s (Win32=%lu)\n", __LINE__, #expr, GetLastError()); \
    result = 1; goto cleanup; } } while (0)

static const ResourceData *find_pid(const ResourceBatch *batch, DWORD pid)
{
    size_t i;
    for (i = 0; i < batch->count; ++i) {
        if (batch->items[i].pid == pid) { return &batch->items[i]; }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    ResourceCollector *collector = NULL;
    ResourceBatch held = {0}, batch = {0};
    ResourceCollectorInfo info;
    ResourceCollectorConfig invalid_config = {0};
    const ResourceData *row;
    char *saved_name = NULL;
    uint32_t error;
    DWORD handles_before = 0, handles_after = 0;
    PROCESS_MEMORY_COUNTERS_EX memory_before, memory_after;
    PROCESS_INFORMATION child;
    STARTUPINFOW startup;
    WCHAR path[MAX_PATH], command[MAX_PATH + 32];
    DWORD child_pid = 0;
    int i, result = 0;
    memset(&child, 0, sizeof(child));
    if (argc > 1 && strcmp(argv[1], "--child") == 0) {
        Sleep(60000); /* Parent always terminates; upper bound if parent crashes. */
        return 0;
    }
    CHECK(resource_collector_create(&invalid_config, &collector, &error) ==
        RESOURCE_INVALID_ARGUMENT);
    CHECK(collector == NULL);
    CHECK(resource_collector_create(NULL, &collector, &error) == RESOURCE_OK);
    CHECK(resource_collector_collect(collector, &held, &error) == RESOURCE_OK);
    row = find_pid(&held, GetCurrentProcessId());
    CHECK(row != NULL);
    CHECK((row->valid_fields & RESOURCE_VALID_MEMORY) != 0);
    CHECK((row->valid_fields & RESOURCE_VALID_IDENTITY) != 0);
    CHECK(row->cpu_status == RESOURCE_DELTA_FIRST_SAMPLE && isnan(row->cpu_percent));
    CHECK(row->process_name_utf8 != NULL);
    saved_name = _strdup(row->process_name_utf8);
    CHECK(saved_name != NULL);
    /* Passing a still-owned batch is rejected without discarding its storage. */
    CHECK(resource_collector_collect(collector, &held, &error) == RESOURCE_INVALID_ARGUMENT);
    CHECK(strcmp(row->process_name_utf8, saved_name) == 0);

    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    CHECK(GetModuleFileNameW(NULL, path, MAX_PATH) != 0);
    CHECK(swprintf_s(command, sizeof(command) / sizeof(command[0]),
        L"\"%ls\" --child", path) > 0);
    CHECK(CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, NULL, &startup, &child));
    child_pid = child.dwProcessId;
    CloseHandle(child.hThread);
    child.hThread = NULL;
    Sleep(50);
    CHECK(resource_collector_collect(collector, &batch, &error) == RESOURCE_OK);
    row = find_pid(&batch, child_pid);
    CHECK(row != NULL);
    CHECK(row->cpu_status == RESOURCE_DELTA_FIRST_SAMPLE);
    CHECK((row->valid_fields & RESOURCE_VALID_MEMORY) != 0);
    resource_batch_free(&batch);
    Sleep(80);
    CHECK(resource_collector_collect(collector, &batch, &error) == RESOURCE_OK);
    row = find_pid(&batch, child_pid);
    CHECK(row != NULL && row->cpu_status == RESOURCE_DELTA_VALID);
    CHECK(row->io_read_status == RESOURCE_DELTA_VALID);
    CHECK(row->cpu_interval_seconds > 0.0);
    resource_batch_free(&batch);
    CHECK(TerminateProcess(child.hProcess, 0));
    CHECK(WaitForSingleObject(child.hProcess, 5000) == WAIT_OBJECT_0);
    CloseHandle(child.hProcess);
    child.hProcess = NULL;
    CHECK(resource_collector_collect(collector, &batch, &error) == RESOURCE_OK);
    CHECK(find_pid(&batch, child_pid) == NULL);
    resource_collector_get_info(collector, &info);
    CHECK(info.tracked_processes == batch.count);
    resource_batch_free(&batch);

    /* Warm up heap/API paths before measuring handle and state stability. */
    for (i = 0; i < 8; ++i) {
        CHECK(resource_collector_collect(collector, &batch, &error) == RESOURCE_OK);
        resource_batch_free(&batch);
    }
    memset(&memory_before, 0, sizeof(memory_before));
    memset(&memory_after, 0, sizeof(memory_after));
    memory_before.cb = sizeof(memory_before);
    memory_after.cb = sizeof(memory_after);
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
    CHECK(GetProcessMemoryInfo(GetCurrentProcess(),
        (PROCESS_MEMORY_COUNTERS *)&memory_before, sizeof(memory_before)));
    for (i = 0; i < 200; ++i) {
        CHECK(resource_collector_collect(collector, &batch, &error) == RESOURCE_OK);
        resource_collector_get_info(collector, &info);
        CHECK(info.tracked_processes == batch.count);
        row = find_pid(&batch, GetCurrentProcessId());
        CHECK(row != NULL && (row->valid_fields & RESOURCE_VALID_MEMORY) != 0);
        resource_batch_free(&batch);
        CHECK(batch.items == NULL && batch.count == 0);
        Sleep(5);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
    CHECK(GetProcessMemoryInfo(GetCurrentProcess(),
        (PROCESS_MEMORY_COUNTERS *)&memory_after, sizeof(memory_after)));
    CHECK(handles_after == handles_before);
    printf("Windows loop: 200 batches, handles %lu -> %lu, private bytes %zu -> %zu, "
        "current states %zu, CPUs %lu\n", handles_before, handles_after,
        memory_before.PrivateUsage, memory_after.PrivateUsage,
        info.tracked_processes, (unsigned long)info.logical_processor_count);

    resource_collector_destroy(collector);
    collector = NULL;
    row = find_pid(&held, GetCurrentProcessId());
    CHECK(row != NULL && strcmp(row->process_name_utf8, saved_name) == 0);
    resource_batch_free(&held);
    resource_batch_free(&held);
    CHECK(held.items == NULL && held.count == 0);
    printf("PASS Windows: %u checks (real new process/exit, ownership, state/handle loop)\n", checks);
cleanup:
    if (child.hThread != NULL) { CloseHandle(child.hThread); }
    if (child.hProcess != NULL) {
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hProcess);
    }
    resource_batch_free(&batch);
    resource_batch_free(&held);
    resource_collector_destroy(collector);
    free(saved_name);
    return result;
}
