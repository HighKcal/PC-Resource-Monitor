/* Toolhelp로 한 회차를 열거하고 프로세스별 API 결과를 순수 계산부에 전달한다.
 * 수집기는 이전 카운터·신원 상태를 보관하며, 반환 배치와 표시 메타데이터는 각각 독립 소유다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include "resource_collector.h"
#include "resource_display.h"
#include "resource_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct ResourceCollector {
    RcState *states;
    size_t state_count;
    int64_t qpc_frequency;
    uint32_t cpu_count;
    double max_interval;
    uint64_t successful_batches;
};

static uint64_t filetime_value(FILETIME value)
{
    return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
}

static uint64_t utc_now(void)
{
    FILETIME value;
    GetSystemTimeAsFileTime(&value);
    return filetime_value(value);
}

static int64_t qpc_now(void)
{
    LARGE_INTEGER value;
    return QueryPerformanceCounter(&value) ? value.QuadPart : 0;
}

static void clear_states(ResourceCollector *collector)
{
    free(collector->states);
    collector->states = NULL;
    collector->state_count = 0;
}

static int compare_states(const void *left, const void *right)
{
    const RcState *a = (const RcState *)left;
    const RcState *b = (const RcState *)right;
    return (a->pid > b->pid) - (a->pid < b->pid);
}

static const RcState *find_state(const ResourceCollector *collector, uint32_t pid)
{
    RcState key;
    if (collector->state_count == 0) {
        return NULL;
    }
    memset(&key, 0, sizeof(key));
    key.pid = pid;
    return (const RcState *)bsearch(&key, collector->states,
        collector->state_count, sizeof(*collector->states), compare_states);
}

static ResourceStatus utf8_name(const WCHAR *wide, char **out, DWORD *error)
{
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
        NULL, 0, NULL, NULL);
    char *name;
    *out = NULL;
    *error = 0;
    if (length == 0) {
        *error = GetLastError();
        return RESOURCE_OK; /* 해당 필드의 실패이며 회차 전체 실패는 아니다. */
    }
    name = (char *)malloc((size_t)length);
    if (name == NULL) {
        return RESOURCE_OUT_OF_MEMORY;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
            name, length, NULL, NULL) == 0) {
        *error = GetLastError();
        free(name);
        return RESOURCE_OK;
    }
    *out = name;
    return RESOURCE_OK;
}

static ResourceStatus read_process(uint32_t pid, RcRawSample *raw, char **name,
    DWORD *open_error, DWORD *times_error, DWORD *memory_error, DWORD *io_error,
    DWORD *name_error, ResourceDisplayInfo *display)
{
    HANDLE process;
    FILETIME creation, exit_time, kernel, user;
    PROCESS_MEMORY_COUNTERS memory;
    IO_COUNTERS io;
    uint64_t kernel_value, user_value;
    WCHAR image_path[32768];
    DWORD path_length = sizeof(image_path) / sizeof(image_path[0]);
    ResourceStatus status = RESOURCE_OK;
    memset(raw, 0, sizeof(*raw));
    raw->pid = pid;
    *name = NULL;
    *open_error = *times_error = *memory_error = *io_error = *name_error = 0;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == NULL) {
        *open_error = GetLastError();
    } else {
        if (display) display->attempted_fields |= RESOURCE_QUERY_TIMES;
        if (GetProcessTimes(process, &creation, &exit_time, &kernel, &user)) {
            raw->cpu_qpc = qpc_now();
            raw->creation_100ns = filetime_value(creation);
            /* 생성시각 0으로는 인스턴스 신원을 확인할 수 없다. */
            raw->identity_valid = raw->creation_100ns != 0;
            kernel_value = filetime_value(kernel);
            user_value = filetime_value(user);
            if (UINT64_MAX - kernel_value >= user_value) {
                raw->cpu_100ns = kernel_value + user_value;
                raw->cpu_valid = 1;
            }
        } else {
            *times_error = GetLastError();
        }
        memset(&memory, 0, sizeof(memory));
        memory.cb = sizeof(memory);
        if (display) display->attempted_fields |= RESOURCE_QUERY_MEMORY;
        if (GetProcessMemoryInfo(process, &memory, sizeof(memory))) {
            raw->memory_valid = 1;
            raw->working_set_bytes = (uint64_t)memory.WorkingSetSize;
        } else {
            *memory_error = GetLastError();
        }
        if (display) display->attempted_fields |= RESOURCE_QUERY_IO;
        if (GetProcessIoCounters(process, &io)) {
            raw->io_qpc = qpc_now();
            raw->io_valid = 1;
            raw->read_bytes = io.ReadTransferCount;
            raw->write_bytes = io.WriteTransferCount;
        } else {
            *io_error = GetLastError();
        }
        /* 목록 이름은 PID가 재사용되기 전의 종료된 인스턴스 이름일 수 있다.
         * 모든 카운터를 읽은 바로 그 핸들에서 실행 파일명을 구한다. */
        if (display) display->attempted_fields |= RESOURCE_QUERY_NAME;
        if (QueryFullProcessImageNameW(process, 0, image_path, &path_length)) {
            const WCHAR *basename = image_path;
            const WCHAR *cursor;
            for (cursor = image_path; *cursor != L'\0'; ++cursor) {
                if (*cursor == L'\\' || *cursor == L'/') {
                    basename = cursor + 1;
                }
            }
            status = utf8_name(basename, name, name_error);
        } else {
            *name_error = GetLastError();
        }
        if (display && (*times_error || *memory_error || *io_error || *name_error)) {
            DWORD exit_code = STILL_ACTIVE;
            display->attempted_fields |= RESOURCE_QUERY_EXIT;
            if (GetExitCodeProcess(process, &exit_code)) {
                display->exit_code = exit_code;
                display->exit_observed = exit_code != STILL_ACTIVE;
            } else {
                display->exit_query_error = GetLastError();
            }
        }
        CloseHandle(process);
    }
    raw->observation_qpc = qpc_now();
    raw->utc_100ns = utc_now();
    return status;
}

ResourceStatus resource_collector_create(const ResourceCollectorConfig *config,
    ResourceCollector **out, uint32_t *win32_error)
{
    LARGE_INTEGER frequency;
    DWORD cpu_count;
    ResourceCollector *collector;
    double max_interval = config == NULL ? 15.0 : config->max_interval_seconds;
    if (win32_error != NULL) {
        *win32_error = 0;
    }
    if (out == NULL) {
        return RESOURCE_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (!isfinite(max_interval) || max_interval <= 0.0) {
        return RESOURCE_INVALID_ARGUMENT;
    }
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        if (win32_error != NULL) {
            *win32_error = GetLastError();
        }
        return RESOURCE_PLATFORM_ERROR;
    }
    cpu_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpu_count == 0) {
        if (win32_error != NULL) {
            *win32_error = GetLastError();
        }
        return RESOURCE_PLATFORM_ERROR;
    }
    collector = (ResourceCollector *)calloc(1, sizeof(*collector));
    if (collector == NULL) {
        return RESOURCE_OUT_OF_MEMORY;
    }
    collector->qpc_frequency = frequency.QuadPart;
    collector->cpu_count = cpu_count;
    collector->max_interval = max_interval;
    *out = collector;
    return RESOURCE_OK;
}

void resource_batch_free(ResourceBatch *batch)
{
    size_t i;
    if (batch == NULL) {
        return;
    }
    for (i = 0; i < batch->count; ++i) {
        free((void *)batch->items[i].process_name_utf8);
    }
    free(batch->items);
    memset(batch, 0, sizeof(*batch));
}

void resource_display_batch_free(ResourceDisplayBatch *display)
{
    size_t i;
    if (display == NULL) return;
    for (i = 0; i < display->count; ++i)
        free((void *)display->items[i].snapshot_name_utf8);
    free(display->items);
    memset(display, 0, sizeof(*display));
}

ResourceStatus resource_collector_collect(ResourceCollector *collector,
    ResourceBatch *out, uint32_t *win32_error)
{
    return resource_collector_collect_display(collector, out, NULL, win32_error);
}

ResourceStatus resource_collector_collect_display(ResourceCollector *collector,
    ResourceBatch *out, ResourceDisplayBatch *display, uint32_t *win32_error)
{
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    PROCESSENTRY32W entry;
    RcState *next_states = NULL;
    size_t capacity = 0;
    ResourceStatus status = RESOURCE_OK;
    DWORD error = 0;
    DWORD foreground_pid = 0;
    HWND foreground;
    BOOL has_entry;
    if (win32_error != NULL) {
        *win32_error = 0;
    }
    if (collector == NULL || out == NULL || out->items != NULL || out->count != 0 ||
            (display && (display->items != NULL || display->count != 0))) {
        return RESOURCE_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->started_at_utc_100ns = utc_now();
    out->logical_processor_count = collector->cpu_count;
    foreground = GetForegroundWindow();
    if (foreground != NULL &&
            GetWindowThreadProcessId(foreground, &foreground_pid) == 0) {
        foreground_pid = 0;
    }
    out->foreground_pid = foreground_pid;
    out->foreground_observed_at_utc_100ns = utc_now();
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        status = RESOURCE_PLATFORM_ERROR;
        goto failed;
    }
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    has_entry = Process32FirstW(snapshot, &entry);
    if (!has_entry && GetLastError() != ERROR_NO_MORE_FILES) {
        error = GetLastError();
        status = RESOURCE_PLATFORM_ERROR;
        goto failed;
    }
    while (has_entry) {
        ResourceData *item;
        ResourceDisplayInfo *display_item = NULL;
        RcRawSample raw;
        DWORD open_error, times_error, memory_error, io_error, name_error;
        char *name;
        if (out->count == capacity) {
            size_t new_capacity = capacity == 0 ? 128 : capacity * 2;
            ResourceData *new_items;
            RcState *new_states;
            if (new_capacity < capacity ||
                    new_capacity > SIZE_MAX / sizeof(*new_items) ||
                    new_capacity > SIZE_MAX / sizeof(*new_states) ||
                    (display && new_capacity > SIZE_MAX / sizeof(*display->items))) {
                status = RESOURCE_OUT_OF_MEMORY;
                goto failed;
            }
            new_items = (ResourceData *)realloc(out->items,
                new_capacity * sizeof(*new_items));
            if (new_items == NULL) {
                status = RESOURCE_OUT_OF_MEMORY;
                goto failed;
            }
            out->items = new_items;
            new_states = (RcState *)realloc(next_states,
                new_capacity * sizeof(*new_states));
            if (new_states == NULL) {
                status = RESOURCE_OUT_OF_MEMORY;
                goto failed;
            }
            next_states = new_states;
            if (display) {
                ResourceDisplayInfo *new_display = (ResourceDisplayInfo *)realloc(
                    display->items, new_capacity * sizeof(*new_display));
                if (new_display == NULL) {
                    status = RESOURCE_OUT_OF_MEMORY;
                    goto failed;
                }
                display->items = new_display;
            }
            capacity = new_capacity;
        }
        if (display) {
            display_item = &display->items[out->count];
            memset(display_item, 0, sizeof(*display_item));
            display_item->pid = entry.th32ProcessID;
            display_item->special_process = entry.th32ProcessID == 0 ?
                RESOURCE_SPECIAL_IDLE : entry.th32ProcessID == 4 ?
                RESOURCE_SPECIAL_SYSTEM : RESOURCE_SPECIAL_NONE;
        }
        status = read_process(entry.th32ProcessID, &raw, &name, &open_error,
            &times_error, &memory_error, &io_error, &name_error, display_item);
        if (status != RESOURCE_OK) {
            goto failed;
        }
        if (display_item) {
            display_item->creation_time_utc_100ns = raw.identity_valid ? raw.creation_100ns : 0;
            display_item->measured_at_utc_100ns = raw.utc_100ns;
            if (name) display_item->name_source = RESOURCE_NAME_HANDLE;
            else if (entry.szExeFile[0]) {
                char *snapshot_name = NULL;
                DWORD snapshot_error = 0;
                status = utf8_name(entry.szExeFile, &snapshot_name, &snapshot_error);
                if (status != RESOURCE_OK) goto failed;
                display_item->snapshot_name_utf8 = snapshot_name;
                display_item->snapshot_name_error = snapshot_error;
                if (snapshot_name) display_item->name_source = RESOURCE_NAME_LIST;
            }
        }
        item = &out->items[out->count];
        rc_core_update(&raw, find_state(collector, raw.pid),
            collector->qpc_frequency, collector->cpu_count, collector->max_interval,
            &next_states[out->count], item);
        item->process_name_utf8 = name;
        item->open_error = open_error;
        item->times_error = times_error;
        item->memory_error = memory_error;
        item->io_error = io_error;
        item->name_error = name_error;
        if (name != NULL) {
            item->valid_fields |= RESOURCE_VALID_NAME;
        }
        item->foreground = rc_foreground_for_pid(raw.pid, foreground_pid);
        if (item->foreground != RESOURCE_FOREGROUND_UNKNOWN) {
            item->valid_fields |= RESOURCE_VALID_FOREGROUND;
        }
        ++out->count;
        if (display) display->count = out->count;
        has_entry = Process32NextW(snapshot, &entry);
        if (!has_entry) {
            error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                status = RESOURCE_PLATFORM_ERROR;
                goto failed;
            }
        }
    }
    /* 전체 열거가 끝난 뒤에만 다음 카운터 상태를 채택한다. */
    CloseHandle(snapshot);
    clear_states(collector);
    if (out->count > 1) {
        qsort(next_states, out->count, sizeof(*next_states), compare_states);
    }
    collector->states = next_states;
    collector->state_count = out->count;
    ++collector->successful_batches;
    out->finished_at_utc_100ns = utc_now();
    return RESOURCE_OK;

failed:
    if (snapshot != INVALID_HANDLE_VALUE) {
        CloseHandle(snapshot);
    }
    free(next_states);
    resource_batch_free(out);
    resource_display_batch_free(display);
    clear_states(collector);
    if (win32_error != NULL) {
        *win32_error = error;
    }
    return status;
}

void resource_collector_destroy(ResourceCollector *collector)
{
    if (collector != NULL) {
        clear_states(collector);
        free(collector);
    }
}

void resource_collector_get_info(const ResourceCollector *collector,
    ResourceCollectorInfo *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        if (collector != NULL) {
            out->tracked_processes = collector->state_count;
            out->logical_processor_count = collector->cpu_count;
            out->successful_batches = collector->successful_batches;
        }
    }
}
