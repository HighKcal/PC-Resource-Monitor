#include "resource_core.h"

#include <math.h>
#include <string.h>

static ResourceDeltaStatus interval_status(int64_t current, int64_t previous,
    int64_t frequency, double limit, double *seconds)
{
    double interval;
    if (frequency <= 0 || previous <= 0 || current <= previous) {
        return RESOURCE_DELTA_CLOCK_INVALID;
    }
    interval = (double)(current - previous) / (double)frequency;
    if (!isfinite(interval) || interval <= 0.0) {
        return RESOURCE_DELTA_CLOCK_INVALID;
    }
    if (interval > limit) {
        return RESOURCE_DELTA_LONG_GAP;
    }
    *seconds = interval;
    return RESOURCE_DELTA_VALID;
}

ResourceForeground rc_foreground_for_pid(uint32_t pid, uint32_t foreground_pid)
{
    if (foreground_pid == 0) {
        return RESOURCE_FOREGROUND_UNKNOWN;
    }
    return pid == foreground_pid ? RESOURCE_FOREGROUND : RESOURCE_BACKGROUND;
}

void rc_core_update(const RcRawSample *raw, const RcState *previous,
    int64_t frequency, uint32_t cpu_count, double max_interval,
    RcState *next, ResourceData *out)
{
    ResourceDeltaStatus identity_status;
    ResourceDeltaStatus io_status;
    double seconds = NAN;
    memset(out, 0, sizeof(*out));
    memset(next, 0, sizeof(*next));
    out->pid = raw->pid;
    out->measured_at_utc_100ns = raw->utc_100ns;
    out->cpu_percent = NAN;
    out->io_read_bytes_per_second = NAN;
    out->io_write_bytes_per_second = NAN;
    out->elapsed_seconds = NAN;
    out->observation_interval_seconds = NAN;
    out->cpu_interval_seconds = NAN;
    out->io_interval_seconds = NAN;
    out->foreground = RESOURCE_FOREGROUND_UNKNOWN;
    next->pid = raw->pid;
    next->identity_valid = raw->identity_valid;
    next->creation_100ns = raw->creation_100ns;
    next->observation_qpc = raw->observation_qpc;
    next->cpu_valid = raw->identity_valid && raw->cpu_valid && raw->cpu_qpc > 0;
    next->io_valid = raw->identity_valid && raw->io_valid && raw->io_qpc > 0;
    next->cpu_100ns = raw->cpu_100ns;
    next->read_bytes = raw->read_bytes;
    next->write_bytes = raw->write_bytes;
    next->cpu_qpc = raw->cpu_qpc;
    next->io_qpc = raw->io_qpc;
    if (raw->memory_valid) {
        out->valid_fields |= RESOURCE_VALID_MEMORY;
        out->working_set_bytes = raw->working_set_bytes;
    }
    if (!raw->identity_valid) {
        identity_status = RESOURCE_DELTA_IDENTITY_UNAVAILABLE;
    } else {
        out->valid_fields |= RESOURCE_VALID_IDENTITY;
        out->creation_time_utc_100ns = raw->creation_100ns;
        if (raw->utc_100ns >= raw->creation_100ns) {
            out->elapsed_seconds =
                (double)(raw->utc_100ns - raw->creation_100ns) / 10000000.0;
            out->valid_fields |= RESOURCE_VALID_ELAPSED;
        }
        if (previous == NULL || previous->pid != raw->pid) {
            identity_status = RESOURCE_DELTA_FIRST_SAMPLE;
        } else if (!previous->identity_valid) {
            identity_status = RESOURCE_DELTA_IDENTITY_UNAVAILABLE;
        } else if (previous->creation_100ns != raw->creation_100ns) {
            identity_status = RESOURCE_DELTA_IDENTITY_CHANGED;
        } else {
            identity_status = RESOURCE_DELTA_VALID;
            if (interval_status(raw->observation_qpc, previous->observation_qpc,
                    frequency, max_interval, &seconds) == RESOURCE_DELTA_VALID) {
                out->observation_interval_seconds = seconds;
                out->valid_fields |= RESOURCE_VALID_OBSERVATION_INTERVAL;
            }
        }
    }
    out->cpu_status = identity_status;
    out->io_read_status = identity_status;
    out->io_write_status = identity_status;
    if (identity_status != RESOURCE_DELTA_VALID) {
        return;
    }

    if (!raw->cpu_valid || !previous->cpu_valid) {
        out->cpu_status = RESOURCE_DELTA_QUERY_FAILED;
    } else if (cpu_count == 0) {
        out->cpu_status = RESOURCE_DELTA_CLOCK_INVALID;
    } else {
        out->cpu_status = interval_status(raw->cpu_qpc, previous->cpu_qpc,
            frequency, max_interval, &seconds);
        if (out->cpu_status == RESOURCE_DELTA_VALID) {
            out->cpu_interval_seconds = seconds;
            if (raw->cpu_100ns < previous->cpu_100ns) {
                out->cpu_status = RESOURCE_DELTA_COUNTER_ROLLBACK;
            } else {
                out->cpu_percent =
                    ((double)(raw->cpu_100ns - previous->cpu_100ns) / 10000000.0)
                    / seconds / (double)cpu_count * 100.0;
                out->valid_fields |= RESOURCE_VALID_CPU;
            }
        }
    }
    if (!raw->io_valid || !previous->io_valid) {
        out->io_read_status = RESOURCE_DELTA_QUERY_FAILED;
        out->io_write_status = RESOURCE_DELTA_QUERY_FAILED;
        return;
    }
    io_status = interval_status(raw->io_qpc, previous->io_qpc,
        frequency, max_interval, &seconds);
    out->io_read_status = io_status;
    out->io_write_status = io_status;
    if (io_status == RESOURCE_DELTA_VALID) {
        out->io_interval_seconds = seconds;
        if (raw->read_bytes < previous->read_bytes) {
            out->io_read_status = RESOURCE_DELTA_COUNTER_ROLLBACK;
        } else {
            out->io_read_bytes_per_second =
                (double)(raw->read_bytes - previous->read_bytes) / seconds;
            out->valid_fields |= RESOURCE_VALID_IO_READ;
        }
        if (raw->write_bytes < previous->write_bytes) {
            out->io_write_status = RESOURCE_DELTA_COUNTER_ROLLBACK;
        } else {
            out->io_write_bytes_per_second =
                (double)(raw->write_bytes - previous->write_bytes) / seconds;
            out->valid_fields |= RESOURCE_VALID_IO_WRITE;
        }
    }
}

const char *resource_delta_status_string(ResourceDeltaStatus status)
{
    switch (status) {
    case RESOURCE_DELTA_VALID: return "valid";
    case RESOURCE_DELTA_FIRST_SAMPLE: return "first_sample";
    case RESOURCE_DELTA_IDENTITY_UNAVAILABLE: return "identity_unavailable";
    case RESOURCE_DELTA_IDENTITY_CHANGED: return "identity_changed";
    case RESOURCE_DELTA_QUERY_FAILED: return "query_failed";
    case RESOURCE_DELTA_CLOCK_INVALID: return "clock_invalid";
    case RESOURCE_DELTA_LONG_GAP: return "long_gap";
    case RESOURCE_DELTA_COUNTER_ROLLBACK: return "counter_rollback";
    default: return "unknown";
    }
}
