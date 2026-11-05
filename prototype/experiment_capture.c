/* 매 회차의 부재·결측·시간 단절을 기록하고 검증된 완료창을 실험 근거로 복사한다.
 * 현재 Track은 빌리며 다음 Track만 준비한다. transaction과 view 수명은 coordinator가 관리한다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include "experiment_internal.h"
#include "experiment_retention.h"
#include "monitor_source.h"

static const unsigned metric_bits[4] = {RESOURCE_VALID_CPU, RESOURCE_VALID_MEMORY, RESOURCE_VALID_IO_READ,
                                        RESOURCE_VALID_IO_WRITE};

int exp_capture_gap(Experiment *e, uint64_t utc, uint64_t session, uint32_t pid, uint64_t creation, unsigned mask,
               const char *reason)
{
    return exp_store_record_operation(e, 1, utc, session, pid, creation, mask, reason, NULL) &&
           exp_reduce_gap(e, utc, session, pid, creation, mask, reason);
}
int exp_capture_window(Experiment *e, int64_t source_window, int64_t source_run, int64_t cutoff, uint64_t session,
                        uint32_t pid, uint64_t creation, const char *name, const char *path,
                        const MonitorMetricReport *reports, int feedback, uint64_t feedback_expires,
                        unsigned interrupted)
{
    int64_t id;
    unsigned m;
    if (!exp_store_window(e, source_window, source_run, cutoff, session, pid, creation, name, path,
                          reports, feedback, feedback_expires, &id))
        return 0;
    for (m = 0; m < 4; ++m) {
        if (!exp_store_metric(e, id, m, &reports[m], !(interrupted & (1u << m))) ||
            !exp_reduce_decision(e, id, session, pid, creation, name, path, m, &reports[m], EXPERIMENT_PERSONALIZED) ||
            !exp_reduce_decision(e, id, session, pid, creation, name, path, m, &reports[m], EXPERIMENT_FIXED))
            return 0;
    }
    return 1;
}
static unsigned bad_metrics(const ResourceData *r)
{
    double values[4] = {r->cpu_percent, (double)r->working_set_bytes, r->io_read_bytes_per_second,
                        r->io_write_bytes_per_second};
    double intervals[4] = {r->cpu_interval_seconds, r->observation_interval_seconds, r->io_interval_seconds,
                           r->io_interval_seconds};
    unsigned m, mask = 0;
    for (m = 0; m < 4; ++m)
        if (!(r->valid_fields & metric_bits[m]) || !isfinite(values[m]) || values[m] < 0 || !isfinite(intervals[m]) ||
            intervals[m] <= 0 || intervals[m] > 15)
            mask |= 1u << m;
    return mask;
}
/* 실험 DB transaction이 열린 상태에서 이전 Track 순서대로 부재 경계를 기록한다. */
int exp_capture_absent_gaps(Experiment *e, const ResourceBatch *batch)
{
    size_t track_index, batch_index;
    for (track_index = 0; track_index < e->track_count; ++track_index) {
        Track *prior_track = &e->tracks[track_index];
        if (!prior_track->identity_valid)
            continue;
        for (batch_index = 0; batch_index < batch->count; ++batch_index)
            if ((batch->items[batch_index].valid_fields & RESOURCE_VALID_IDENTITY) &&
                prior_track->pid == batch->items[batch_index].pid &&
                prior_track->creation == batch->items[batch_index].creation_time_utc_100ns)
                break;
        if (batch_index == batch->count &&
            !exp_capture_gap(e, batch->finished_at_utc_100ns, e->session, prior_track->pid, prior_track->creation, ALL_METRICS,
                 "instance absent or identity changed; outcome unknown"))
            return 0;
    }
    return 1;
}

/* 호출자 소유의 다음 Track을 준비한다. COMMIT 전에는 현재 Track을 교체하지 않는다. */
int exp_capture_continuity(Experiment *e, const ResourceBatch *batch, Track *next_tracks)
{
    size_t batch_index, prior_index;
    for (batch_index = 0; batch_index < batch->count; ++batch_index) {
        const ResourceData *sample = &batch->items[batch_index];
        unsigned interrupted_mask;
        int observation_valid;
        next_tracks[batch_index].pid = sample->pid;
        next_tracks[batch_index].creation = sample->creation_time_utc_100ns;
        next_tracks[batch_index].utc = sample->measured_at_utc_100ns;
        next_tracks[batch_index].identity_valid = (sample->valid_fields & RESOURCE_VALID_IDENTITY) != 0;
        if (!(sample->valid_fields & RESOURCE_VALID_IDENTITY))
            continue;
        for (prior_index = 0; prior_index < e->track_count; ++prior_index)
            if (e->tracks[prior_index].identity_valid && e->tracks[prior_index].pid == sample->pid &&
                e->tracks[prior_index].creation == sample->creation_time_utc_100ns)
                break;
        observation_valid =
            (sample->valid_fields & RESOURCE_VALID_OBSERVATION_INTERVAL) &&
            isfinite(sample->observation_interval_seconds) && sample->observation_interval_seconds > 0 &&
            sample->observation_interval_seconds <= 15 &&
            (prior_index == e->track_count || sample->measured_at_utc_100ns > e->tracks[prior_index].utc);
        interrupted_mask = !observation_valid ? ALL_METRICS : bad_metrics(sample);
        next_tracks[batch_index].interrupted =
            interrupted_mask | (prior_index < e->track_count ? e->tracks[prior_index].interrupted : 0);
        if (interrupted_mask && !exp_capture_gap(e, sample->measured_at_utc_100ns, e->session, sample->pid,
                                     sample->creation_time_utc_100ns, interrupted_mask,
                                     observation_valid ? "missing metric sample; persistence reset"
                                                       : "observation gap or nonincreasing UTC; outcome unknown"))
            return 0;
    }
    return 1;
}

/* source facade가 검증하고 정렬한 전체 view에서 값을 실험 DB로 복사한다.
   중복 PID+생성시각도 기존 첫 일치 행 선택을 유지한다. flag는 진단용이며
   중복 행별 정확한 연결을 보장하지 않는다. view 해제는 coordinator가 담당한다. */
int exp_capture_committed_windows(Experiment *e, int64_t cutoff, const MonitorCommittedWindows *windows,
                                  Track *next_tracks)
{
    size_t i;
    for (i = 0; i < windows->count; ++i) {
        const MonitorCommittedWindow *window = &windows->items[i];
        const MonitorRow *source_row = window->row;
        if (!exp_capture_window(e, window->window_id, window->run_id,
                          cutoff, e->session, source_row->raw.pid, source_row->raw.creation_time_utc_100ns,
                          source_row->name, source_row->path ? source_row->path : "",
                          source_row->metric, source_row->feedback, source_row->feedback_expires_utc_100ns,
                          next_tracks[window->row_index].interrupted))
            return 0;
        next_tracks[window->row_index].interrupted = 0;
    }
    return 1;
}

