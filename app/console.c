#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "resource_collector.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE stop_event;

static BOOL WINAPI on_control(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}

static int parse_number(const char *text, uint32_t *value)
{
    char *end;
    unsigned long parsed;
    if (*text == '\0' || *text == '-') {
        return 0;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > UINT32_MAX) {
        return 0;
    }
    *value = (uint32_t)parsed;
    return 1;
}

static void print_csv_name(const char *name)
{
    putchar('"');
    if (name != NULL) {
        for (; *name != '\0'; ++name) {
            if (*name == '"') {
                putchar('"');
            }
            putchar(*name);
        }
    }
    putchar('"');
}

static void print_double(double value, int valid)
{
    if (valid) {
        printf("%.6f", value);
    } else {
        fputs("NA", stdout);
    }
}

static void print_row(const ResourceData *row)
{
    FILETIME ft;
    SYSTEMTIME st;
    ft.dwLowDateTime = (DWORD)row->measured_at_utc_100ns;
    ft.dwHighDateTime = (DWORD)(row->measured_at_utc_100ns >> 32);
    if (FileTimeToSystemTime(&ft, &st)) {
        printf("%04u-%02u-%02uT%02u:%02u:%02u.%03uZ,",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    } else {
        fputs("NA,", stdout);
    }
    printf("%lu,", (unsigned long)row->pid);
    print_csv_name(row->process_name_utf8);
    printf(",0x%03lx,", (unsigned long)row->valid_fields);
    if (row->valid_fields & RESOURCE_VALID_IDENTITY) {
        printf("%llu", (unsigned long long)row->creation_time_utc_100ns);
    } else {
        fputs("NA", stdout);
    }
    putchar(',');
    print_double(row->cpu_percent, row->valid_fields & RESOURCE_VALID_CPU);
    putchar(',');
    if (row->valid_fields & RESOURCE_VALID_MEMORY) {
        printf("%llu", (unsigned long long)row->working_set_bytes);
    } else {
        fputs("NA", stdout);
    }
    putchar(',');
    print_double(row->io_read_bytes_per_second,
        row->valid_fields & RESOURCE_VALID_IO_READ);
    putchar(',');
    print_double(row->io_write_bytes_per_second,
        row->valid_fields & RESOURCE_VALID_IO_WRITE);
    printf(",%d,", row->foreground);
    print_double(row->elapsed_seconds, row->valid_fields & RESOURCE_VALID_ELAPSED);
    putchar(',');
    print_double(row->observation_interval_seconds,
        row->valid_fields & RESOURCE_VALID_OBSERVATION_INTERVAL);
    putchar(',');
    print_double(row->cpu_interval_seconds, row->valid_fields & RESOURCE_VALID_CPU);
    putchar(',');
    print_double(row->io_interval_seconds,
        row->valid_fields & (RESOURCE_VALID_IO_READ | RESOURCE_VALID_IO_WRITE));
    printf(",%s,%s,%s,%lu,%lu,%lu,%lu\n",
        resource_delta_status_string(row->cpu_status),
        resource_delta_status_string(row->io_read_status),
        resource_delta_status_string(row->io_write_status),
        (unsigned long)row->open_error, (unsigned long)row->times_error,
        (unsigned long)row->memory_error, (unsigned long)row->io_error);
}

int main(int argc, char **argv)
{
    ResourceCollector *collector = NULL;
    uint32_t interval_ms = 3000, count = 0, pid_filter = 0, error = 0;
    uint32_t iteration = 0;
    int i, exit_code = 0;
    ResourceStatus status;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self") == 0) {
            pid_filter = GetCurrentProcessId();
        } else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], &count)) { goto usage; }
        } else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], &interval_ms) || interval_ms == 0 ||
                    interval_ms == INFINITE) { goto usage; }
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], &pid_filter) || pid_filter == 0) { goto usage; }
        } else {
            goto usage;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (stop_event == NULL) {
        fprintf(stderr, "CreateEvent failed: %lu\n", GetLastError());
        return 1;
    }
    if (!SetConsoleCtrlHandler(on_control, TRUE)) {
        fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
        CloseHandle(stop_event);
        return 1;
    }
    status = resource_collector_create(NULL, &collector, &error);
    if (status != RESOURCE_OK) {
        fprintf(stderr, "create failed: status=%d Win32=%lu\n", status,
            (unsigned long)error);
        exit_code = 1;
        goto done;
    }
    puts("measured_utc,pid,name_utf8,valid_fields,creation_utc_100ns,cpu_percent,"
        "working_set_bytes,io_read_Bps,io_write_Bps,foreground,elapsed_seconds,"
        "observation_interval_seconds,cpu_interval_seconds,io_interval_seconds,"
        "cpu_status,read_status,write_status,open_error,times_error,memory_error,io_error");
    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        ResourceBatch batch = {0};
        size_t j;
        status = resource_collector_collect(collector, &batch, &error);
        if (status != RESOURCE_OK) {
            fprintf(stderr, "collect failed: status=%d Win32=%lu\n", status,
                (unsigned long)error);
            exit_code = 1;
            break;
        }
        ++iteration;
        fprintf(stderr, "batch=%lu processes=%zu logical_cpus=%lu foreground_pid=%lu "
            "foreground_utc_100ns=%llu\n", (unsigned long)iteration, batch.count,
            (unsigned long)batch.logical_processor_count,
            (unsigned long)batch.foreground_pid,
            (unsigned long long)batch.foreground_observed_at_utc_100ns);
        for (j = 0; j < batch.count; ++j) {
            if (pid_filter == 0 || batch.items[j].pid == pid_filter) {
                print_row(&batch.items[j]);
            }
        }
        resource_batch_free(&batch);
        fflush(stdout);
        if (count != 0 && iteration >= count) {
            break;
        }
        if (WaitForSingleObject(stop_event, interval_ms) == WAIT_FAILED) {
            fprintf(stderr, "wait failed: %lu\n", GetLastError());
            exit_code = 1;
            break;
        }
    }
done:
    resource_collector_destroy(collector);
    SetConsoleCtrlHandler(on_control, FALSE);
    CloseHandle(stop_event);
    return exit_code;
usage:
    fprintf(stderr, "Usage: resource_console [--count N] [--interval-ms N] "
        "[--pid PID | --self]\nDefaults: count 0 (until Ctrl+C), interval 3000 ms.\n");
    return 2;
}
