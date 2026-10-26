#ifndef RESOURCE_COLLECTOR_H
#define RESOURCE_COLLECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Windows desktop, C17; APIs target _WIN32_WINNT=0x0601.
 * Runtime validated on Windows 11 x64, not on every API-compatible OS.
 * All UTC values: 100 ns since 1601-01-01 UTC. */
typedef struct ResourceCollector ResourceCollector;

typedef enum ResourceStatus {
    RESOURCE_OK = 0,
    RESOURCE_INVALID_ARGUMENT,
    RESOURCE_OUT_OF_MEMORY,
    RESOURCE_PLATFORM_ERROR
} ResourceStatus;

typedef enum ResourceForeground {
    RESOURCE_FOREGROUND_UNKNOWN = -1,
    RESOURCE_BACKGROUND = 0,
    RESOURCE_FOREGROUND = 1
} ResourceForeground;

typedef enum ResourceDeltaStatus {
    RESOURCE_DELTA_VALID = 0,
    RESOURCE_DELTA_FIRST_SAMPLE,
    RESOURCE_DELTA_IDENTITY_UNAVAILABLE,
    RESOURCE_DELTA_IDENTITY_CHANGED,
    RESOURCE_DELTA_QUERY_FAILED,
    RESOURCE_DELTA_CLOCK_INVALID,
    RESOURCE_DELTA_LONG_GAP,
    RESOURCE_DELTA_COUNTER_ROLLBACK
} ResourceDeltaStatus;

enum ResourceValidity {
    RESOURCE_VALID_NAME = 1u << 0,
    RESOURCE_VALID_IDENTITY = 1u << 1,
    RESOURCE_VALID_CPU = 1u << 2,
    RESOURCE_VALID_MEMORY = 1u << 3,
    RESOURCE_VALID_IO_READ = 1u << 4,
    RESOURCE_VALID_IO_WRITE = 1u << 5,
    RESOURCE_VALID_ELAPSED = 1u << 6,
    RESOURCE_VALID_FOREGROUND = 1u << 7,
    RESOURCE_VALID_OBSERVATION_INTERVAL = 1u << 8
};

typedef struct ResourceData {
    uint64_t measured_at_utc_100ns;
    uint32_t pid;
    const char *process_name_utf8; /* Same-handle image basename; batch-owned,
                                  * read-only UTF-8; NULL if unavailable. */
    uint64_t creation_time_utc_100ns; /* Meaningful only with VALID_IDENTITY. */
    uint32_t valid_fields;
    double cpu_percent;             /* Entire system logical CPU capacity = 100%. */
    uint64_t working_set_bytes;     /* Meaningful only with VALID_MEMORY. */
    double io_read_bytes_per_second;
    double io_write_bytes_per_second;
    double elapsed_seconds;         /* Wall-clock UTC now minus process creation. */
    double observation_interval_seconds; /* QPC between successful identities. */
    double cpu_interval_seconds;    /* QPC at CPU counter observations. */
    double io_interval_seconds;     /* QPC at I/O counter observations. */
    ResourceForeground foreground; /* Batch foreground snapshot, not duration. */
    ResourceDeltaStatus cpu_status;
    ResourceDeltaStatus io_read_status;
    ResourceDeltaStatus io_write_status;
    uint32_t open_error;            /* Win32 error, zero on successful API call. */
    uint32_t times_error;
    uint32_t memory_error;
    uint32_t io_error;
    uint32_t name_error;
} ResourceData;

typedef struct ResourceBatch {
    ResourceData *items;            /* Read-only to consumers. */
    size_t count;
    uint64_t started_at_utc_100ns;
    uint64_t finished_at_utc_100ns;
    uint64_t foreground_observed_at_utc_100ns;
    uint32_t foreground_pid;        /* Zero means foreground is unknown. */
    uint32_t logical_processor_count;
} ResourceBatch;

typedef struct ResourceCollectorConfig {
    double max_interval_seconds; /* >0, finite; default 15.0, gap > limit invalid. */
} ResourceCollectorConfig;

typedef struct ResourceCollectorInfo {
    size_t tracked_processes;       /* Current snapshot only; no retained handles. */
    uint32_t logical_processor_count;
    uint64_t successful_batches;
} ResourceCollectorInfo;

/* config == NULL selects defaults. On failure *out == NULL. */
ResourceStatus resource_collector_create(const ResourceCollectorConfig *config,
    ResourceCollector **out, uint32_t *win32_error);

/* One pass; no waiting. *out must be zero-initialized or previously freed.
 * A platform/allocation failure empties the output and all baselines. Invalid
 * arguments are rejected without modifying existing output/state. Per-process API
 * failures remain in the successful batch as field validity/errors.
 * One collector must not be used concurrently or recursively. */
ResourceStatus resource_collector_collect(ResourceCollector *collector,
    ResourceBatch *out, uint32_t *win32_error);

/* Frees only this batch's array and names; zeroes *batch. Idempotent.
 * A batch remains valid across later collects and even collector destruction. */
void resource_batch_free(ResourceBatch *batch);
void resource_collector_destroy(ResourceCollector *collector);
void resource_collector_get_info(const ResourceCollector *collector,
    ResourceCollectorInfo *out);
const char *resource_delta_status_string(ResourceDeltaStatus status);

#ifdef __cplusplus
}
#endif
#endif
