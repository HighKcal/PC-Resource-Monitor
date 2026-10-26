#ifndef RESOURCE_CORE_H
#define RESOURCE_CORE_H

#include "resource_collector.h"

/* Private seam: production and controlled tests share this state transition. */
typedef struct RcRawSample {
    uint32_t pid;
    uint64_t utc_100ns;
    uint64_t creation_100ns;
    int identity_valid;
    int cpu_valid;
    int io_valid;
    int memory_valid;
    uint64_t cpu_100ns;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t working_set_bytes;
    int64_t observation_qpc;
    int64_t cpu_qpc;
    int64_t io_qpc;
} RcRawSample;

typedef struct RcState {
    uint32_t pid;
    uint64_t creation_100ns;
    int identity_valid;
    int cpu_valid;
    int io_valid;
    uint64_t cpu_100ns;
    uint64_t read_bytes;
    uint64_t write_bytes;
    int64_t observation_qpc;
    int64_t cpu_qpc;
    int64_t io_qpc;
} RcState;

void rc_core_update(const RcRawSample *raw, const RcState *previous,
    int64_t frequency, uint32_t cpu_count, double max_interval,
    RcState *next, ResourceData *out);
ResourceForeground rc_foreground_for_pid(uint32_t pid, uint32_t foreground_pid);

#endif
