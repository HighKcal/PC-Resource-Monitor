/* 커밋된 전체 snapshot으로 알림 후보·재발·전달 근거를 관리한다. 읽기 시작점은
 * notification_policy_observe의 일반 표본 검증 → 완료창 전이와 begin → dispatch → result다.
 * 이 파일은 Shell을 호출하지 않고 값 복사만 보관한다. 호출자가 모든 접근을 직렬화한다. */
#include <limits.h>
#include <math.h>
#include <string.h>
#include "../detection/monitor_policy.h"
#include "notification_policy.h"

#define ROW_ABSENT SIZE_MAX
#define ROW_AMBIGUOUS (SIZE_MAX - 1)

static const unsigned metric_validity[MONITOR_METRICS] = {
    RESOURCE_VALID_CPU, RESOURCE_VALID_MEMORY, RESOURCE_VALID_IO_READ, RESOURCE_VALID_IO_WRITE
};
static const uint64_t recurrence_ms = NOTIFICATION_RECURRENCE_MS;

static uint64_t after(uint64_t now, uint64_t interval)
{
    return now > UINT64_MAX - interval ? UINT64_MAX : now + interval;
}

static int recent(uint64_t now, uint64_t then, uint64_t maximum_age)
{
    return now >= then && now - then <= maximum_age;
}

static int known_row(const MonitorRow *row)
{
    return row->raw.pid && row->raw.creation_time_utc_100ns &&
        (row->raw.valid_fields & RESOURCE_VALID_IDENTITY) &&
        !row->raw.open_error && !row->raw.times_error && row->program_id > 0 &&
        row->path && row->path[0] && !row->path_error;
}

static int recent_row(const MonitorRow *row, uint64_t now_utc)
{
    return row->raw.measured_at_utc_100ns && recent(now_utc, row->raw.measured_at_utc_100ns,
        NOTIFICATION_FRESH_MS * (MONITOR_TICKS_PER_SECOND / 1000));
}

static int metric_ready(const MonitorRow *row, unsigned metric)
{
    const MonitorMetricReport *report = &row->metric[metric];
    return (row->raw.valid_fields & metric_validity[metric]) &&
        report->window_start_utc_100ns && report->window_end_utc_100ns > report->window_start_utc_100ns &&
        isfinite(report->current) && isfinite(report->median) && isfinite(report->threshold);
}

static unsigned candidate_metrics(const MonitorRow *row, int fresh_only)
{
    unsigned metric, mask = 0;
    for (metric = 0; metric < MONITOR_METRICS; ++metric) {
        const MonitorMetricReport *report = &row->metric[metric];
        if (report->state == MONITOR_CANDIDATE && report->streak >= BASELINE_CANDIDATE_STREAK &&
            metric_ready(row, metric) &&
            (!fresh_only || report->window_end_utc_100ns == row->raw.measured_at_utc_100ns))
            mask |= 1u << metric;
    }
    return mask;
}

static int suppressed(const MonitorRow *row, uint64_t utc)
{
    return row->feedback != MONITOR_FEEDBACK_CANCEL && row->feedback_expires_utc_100ns > utc;
}

/* Track은 PID 순으로 유지한다. 관측됐지만 신원은 모르는 행의 상태는 남기되,
 * 같은 PID가 여러 행이면 어느 실행 인스턴스인지 확정하지 않는다. */
static unsigned track_position(const NotificationPolicy *policy, uint32_t pid)
{
    unsigned low = 0, high = policy->track_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (policy->tracks[middle].pid < pid) low = middle + 1;
        else high = middle;
    }
    return low;
}

static NotificationTrack *member_track(NotificationPolicy *policy, const NotificationEvent *event,
    const NotificationMember *member)
{
    unsigned pos = track_position(policy, member->pid);
    if (pos == policy->track_count || event->session != policy->session) return NULL;
    NotificationTrack *track = &policy->tracks[pos];
    if (track->pid != member->pid || track->creation != member->creation || track->program != event->program)
        return NULL;
    return track;
}

static NotificationProgram *program_record(NotificationPolicy *policy, int64_t program, int create)
{
    unsigned position = 0;
    while (position < policy->program_count && policy->programs[position].program != program) ++position;
    if (position < policy->program_count) return &policy->programs[position];
    if (!create) return NULL;
    if (position == NOTIFICATION_PROGRAM_LIMIT) {
        uint64_t oldest = UINT64_MAX;
        position = 0;
        for (unsigned i = 0; i < policy->program_count; ++i) {
            uint64_t newest = 0;
            for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric)
                if (policy->programs[i].accepted_at[metric] > newest)
                    newest = policy->programs[i].accepted_at[metric];
            if (newest < oldest) { oldest = newest; position = i; }
        }
        ++policy->stats.history_drops;
    } else ++policy->program_count;
    policy->programs[position] = (NotificationProgram){.program = program};
    return &policy->programs[position];
}

static void add_track(NotificationPolicy *policy, const MonitorRow *row)
{
    unsigned pos = track_position(policy, row->raw.pid);
    if (pos < policy->track_count && policy->tracks[pos].pid == row->raw.pid) return;
    if (policy->track_count == NOTIFICATION_TRACK_LIMIT) {
        ++policy->stats.tracking_drops;
        return;
    }
    memmove(&policy->tracks[pos + 1], &policy->tracks[pos],
        (policy->track_count - pos) * sizeof(policy->tracks[0]));
    policy->tracks[pos] = (NotificationTrack){.pid = row->raw.pid};
    ++policy->track_count;
}

static void retry_metrics(NotificationTrack *track, unsigned mask, uint64_t now)
{
    unsigned metric;
    if (!track) return;
    for (metric = 0; metric < MONITOR_METRICS; ++metric)
        if (mask & (1u << metric)) track->retry_at[metric] = after(now, NOTIFICATION_RETRY_MS);
}

static void retry_omitted(NotificationPolicy *policy, const NotificationEvent *event, uint64_t now)
{
    if (!event->omitted_count) return;
    for (unsigned i = 0; i < policy->track_count; ++i) {
        NotificationTrack *track = &policy->tracks[i];
        if (track->overflow_id != event->id) continue;
        retry_metrics(track, track->overflow_metrics, now);
        track->overflow_id = 0;
        track->overflow_metrics = 0;
    }
}

static void retry_event(NotificationPolicy *policy, const NotificationEvent *event, uint64_t now)
{
    for (unsigned i = 0; i < event->member_count; ++i) {
        const NotificationMember *member = &event->members[i];
        retry_metrics(member_track(policy, event, member), member->metrics, now);
    }
    retry_omitted(policy, event, now);
}

static void remove_queued(NotificationPolicy *policy, unsigned pos)
{
    --policy->count;
    memmove(&policy->queue[pos], &policy->queue[pos + 1],
        (policy->count - pos) * sizeof(policy->queue[0]));
}

static void expire_queued(NotificationPolicy *policy, uint64_t now)
{
    while (policy->count && !recent(now, policy->queue[0].queued_ms, NOTIFICATION_TTL_MS - 1)) {
        const NotificationEvent *event = &policy->queue[0];
        retry_event(policy, event, now);
        ++policy->stats.expired;
        remove_queued(policy, 0);
    }
}

static void copy_basename(char name[NOTIFICATION_NAME_SIZE], const char *path)
{
    const char *base = path, *p;
    size_t length;
    for (p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    length = (size_t)(p - base);
    if (length >= NOTIFICATION_NAME_SIZE) {
        length = NOTIFICATION_NAME_SIZE - 1;
        while (length && ((unsigned char)base[length] & 0xc0u) == 0x80u) --length;
    }
    if (!length) {
        strcpy_s(name, NOTIFICATION_NAME_SIZE, "프로세스");
        return;
    }
    for (size_t i = 0; i < length; ++i)
        name[i] = (unsigned char)base[i] < 0x20u || base[i] == 0x7f ? '?' : base[i];
    name[length] = '\0';
}

static NotificationEvent *pending_program(NotificationPolicy *policy, int64_t program)
{
    if (policy->active.id && !policy->active_sent && policy->active.program == program)
        return &policy->active;
    for (unsigned i = 0; i < policy->count; ++i)
        if (policy->queue[i].program == program) return &policy->queue[i];
    return NULL;
}

static NotificationMember *pending_member(NotificationEvent *event, const NotificationTrack *track)
{
    if (event)
        for (unsigned i = 0; i < event->member_count; ++i)
            if (event->members[i].pid == track->pid && event->members[i].creation == track->creation)
                return &event->members[i];
    return NULL;
}

static void add_member_evidence(NotificationMember *member, const NotificationTrack *track,
    const MonitorRow *row, unsigned mask, const NotificationReason *reason)
{
    member->metrics |= mask;
    for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
        const MonitorMetricReport *report = &row->metric[metric];
        if (!(mask & (1u << metric))) continue;
        member->metric[metric] = (NotificationMetricEvidence){report->window_start_utc_100ns,
            report->window_end_utc_100ns, report->current, report->median, report->threshold,
            report->window_duration_seconds, report->streak};
        member->reason[metric] = reason[metric];
        member->candidate_since[metric] = track->candidate_since[metric] ?
            track->candidate_since[metric] : report->window_start_utc_100ns;
    }
}

static int meaningful_growth(double current, double previous, unsigned metric)
{
    static const double relative[] = {2.0, 1.25, 2.0, 2.0};
    static const double absolute[] = {5.0, 268435456.0, 5242880.0, 5242880.0};
    return current >= previous * relative[metric] && current - previous >= absolute[metric];
}

static int recurrence_quiet(const NotificationProgram *program, unsigned metric, uint64_t now)
{
    return recurrence_ms && program && (program->accepted_metrics & (1u << metric)) &&
        now < after(program->accepted_at[metric], recurrence_ms);
}

static void mark_deferred(NotificationPolicy *policy, NotificationTrack *track, unsigned bit)
{
    if (!(track->deferred & bit)) ++policy->stats.deferred;
    track->deferred |= bit;
}

/* 최초 전달, 재발 대기 해제, 대기 중 추가 증가를 지표별로 구분한다. 추가 증가는
 * 마지막 수락 당시 값과 비교하고 두 연속 완료창에서 확인해야 한다. 결측은 회복이 아니다. */
static NotificationReason delivery_reason(NotificationPolicy *policy, NotificationTrack *track,
    const MonitorMetricReport *report, unsigned metric, uint64_t now)
{
    const unsigned bit = 1u << metric;
    NotificationProgram *program;
    uint64_t previous_end;
    if (!(track->delivered & bit)) return NOTIFICATION_REASON_FIRST;
    if (!recurrence_ms) return NOTIFICATION_REASON_RECURRENCE;
    program = program_record(policy, track->program, 0);
    previous_end = track->growth_end[metric];
    if (report->window_end_utc_100ns <= previous_end) return 0;
    if (!previous_end || report->window_start_utc_100ns != previous_end)
        track->candidate_since[metric] = report->window_start_utc_100ns;
    track->growth_end[metric] = report->window_end_utc_100ns;
    policy->growth_armed = 1;
    if (!recurrence_quiet(program, metric, now))
        return (track->deferred & bit) ? NOTIFICATION_REASON_DEFERRED : NOTIFICATION_REASON_RECURRENCE;
    mark_deferred(policy, track, bit);
    if (!meaningful_growth(report->current, track->reference[metric], metric)) track->growth_count[metric] = 0;
    else if (!track->growth_count[metric] || report->window_start_utc_100ns != previous_end)
        track->growth_count[metric] = 1;
    else track->growth_count[metric] = 2;
    return track->growth_count[metric] == 2 ? NOTIFICATION_REASON_GROWTH : 0;
}

/* 같은 실행 경로의 program_id별 미전달 사건에 인스턴스와 지표 근거를 복사한다.
 * 표시 그룹과 무관하며 상한 밖 구성원은 생략 수·재시도 상태로 남긴다. */
static void enqueue_candidate(NotificationPolicy *policy, NotificationTrack *track, const MonitorRow *row,
    unsigned mask, uint64_t now)
{
    NotificationEvent *event;
    NotificationMember *member;
    NotificationReason reason[MONITOR_METRICS] = {0};
    mask &= ~(track->accepted | track->provisional);
    if (!mask) return;
    event = pending_program(policy, track->program);
    member = pending_member(event, track);
    if (member) mask &= ~member->metrics;
    if (event && track->overflow_id == event->id) mask &= ~track->overflow_metrics;
    for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
        unsigned bit = 1u << metric;
        if (!(mask & bit)) continue;
        if (now >= track->retry_at[metric]) reason[metric] = delivery_reason(policy, track, &row->metric[metric], metric, now);
        if (!reason[metric]) mask &= ~bit;
    }
    if (!mask) return;
    if (!event) {
        if (policy->count == NOTIFICATION_QUEUE_LIMIT || policy->next_id == UINT64_MAX) {
            retry_metrics(track, mask, now);
            ++policy->stats.overflow;
            return;
        }
        event = &policy->queue[policy->count++];
        *event = (NotificationEvent){.id = ++policy->next_id, .queued_ms = now, .session = policy->session,
            .program = track->program};
        copy_basename(event->name, row->path);
        ++policy->stats.queued;
    }
    if (!member && event->member_count == NOTIFICATION_MEMBER_LIMIT) {
        if (track->overflow_id != event->id) {
            track->overflow_id = event->id;
            track->overflow_joined_ms = now;
            track->overflow_metrics = 0;
            ++event->omitted_count;
        }
        track->overflow_metrics |= mask;
        event->omitted_metrics |= mask;
        for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
            if (!(mask & (1u << metric))) continue;
            track->overflow_end[metric] = row->metric[metric].window_end_utc_100ns;
            track->overflow_reason[metric] = reason[metric];
        }
        return;
    }
    if (!member) {
        member = &event->members[event->member_count++];
        *member = (NotificationMember){.pid = track->pid, .creation = track->creation};
    }
    add_member_evidence(member, track, row, mask, reason);
    event->metrics |= mask;
}

static const MonitorRow *current_track_row(const NotificationPolicy *policy, const NotificationTrack *track,
    const MonitorSnapshot *snapshot, const size_t *observed, uint64_t utc)
{
    size_t index = track ? observed[track - policy->tracks] : ROW_ABSENT;
    if (index < snapshot->count) {
        const MonitorRow *row = &snapshot->rows[index];
        if (known_row(row) && recent_row(row, utc) && row->raw.creation_time_utc_100ns == track->creation &&
            row->program_id == track->program) return row;
    }
    return NULL;
}

static void promote_omitted(NotificationPolicy *policy, NotificationEvent *event,
    const MonitorSnapshot *snapshot, const size_t *observed)
{
    while (event->member_count < NOTIFICATION_MEMBER_LIMIT && event->omitted_count) {
        NotificationTrack *earliest = NULL;
        for (unsigned i = 0; i < policy->track_count; ++i) {
            NotificationTrack *track = &policy->tracks[i];
            unsigned copied = track->overflow_metrics;
            const MonitorRow *row;
            if (track->overflow_id != event->id || (earliest && earliest->overflow_joined_ms <= track->overflow_joined_ms))
                continue;
            row = &snapshot->rows[observed[i]];
            for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric)
                if (row->metric[metric].window_end_utc_100ns != track->overflow_end[metric]) copied &= ~(1u << metric);
            /* 증가 확인을 포함한 원인은 처음 복사한 완료창에 속한다. 최신 창으로 바꾸지 않는다. */
            if (copied == track->overflow_metrics) earliest = track;
        }
        if (!earliest) break;
        NotificationMember *member = &event->members[event->member_count++];
        *member = (NotificationMember){.pid = earliest->pid, .creation = earliest->creation};
        add_member_evidence(member, earliest, &snapshot->rows[observed[earliest - policy->tracks]],
            earliest->overflow_metrics, earliest->overflow_reason);
        event->metrics |= earliest->overflow_metrics;
        earliest->overflow_id = 0;
        earliest->overflow_metrics = 0;
        --event->omitted_count;
    }
    event->omitted_metrics = 0;
    if (event->omitted_count) for (unsigned i = 0; i < policy->track_count; ++i)
        if (policy->tracks[i].overflow_id == event->id) event->omitted_metrics |= policy->tracks[i].overflow_metrics;
}

static void reconcile_event(NotificationPolicy *policy, NotificationEvent *event,
    const MonitorSnapshot *snapshot, const size_t *observed, uint64_t utc)
{
    unsigned kept = 0;
    const unsigned had_omissions = event->omitted_count;
    event->metrics = event->omitted_count = event->omitted_metrics = 0;
    for (unsigned i = 0; i < event->member_count; ++i) {
        NotificationMember *member = &event->members[i];
        NotificationTrack *track = member_track(policy, event, member);
        const MonitorRow *row = current_track_row(policy, track, snapshot, observed, utc);
        if (!row) continue;
        if (suppressed(row, utc)) goto purge;
        member->metrics &= candidate_metrics(row, 0);
        for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric)
            if (row->metric[metric].window_end_utc_100ns < member->metric[metric].window_end)
                member->metrics &= ~(1u << metric);
        if (member->metrics) {
            event->metrics |= member->metrics;
            event->members[kept++] = *member;
        }
    }
    event->member_count = kept;
    if (had_omissions) for (unsigned i = 0; i < policy->track_count; ++i) {
        NotificationTrack *track = &policy->tracks[i];
        const MonitorRow *row;
        unsigned mask;
        if (track->overflow_id != event->id) continue;
        row = current_track_row(policy, track, snapshot, observed, utc);
        if (row && suppressed(row, utc)) goto purge;
        mask = row ? track->overflow_metrics & candidate_metrics(row, 0) : 0;
        for (unsigned metric = 0; row && metric < MONITOR_METRICS; ++metric)
            if (row->metric[metric].window_end_utc_100ns < track->overflow_end[metric]) mask &= ~(1u << metric);
        track->overflow_metrics = mask;
        if (mask) {
            ++event->omitted_count;
            event->omitted_metrics |= mask;
        }
        if (!track->overflow_metrics) track->overflow_id = 0;
    }
    if (event->member_count < NOTIFICATION_MEMBER_LIMIT && event->omitted_count)
        promote_omitted(policy, event, snapshot, observed);
    if (!event->metrics) ++policy->stats.invalidated;
    return;
purge:
    event->metrics = 0;
    ++policy->stats.feedback_purged;
}

static void reconcile_pending(NotificationPolicy *policy, const MonitorSnapshot *snapshot,
    const size_t *observed, uint64_t utc)
{
    unsigned i = 0;
    if (policy->active.id && !policy->active_sent) {
        reconcile_event(policy, &policy->active, snapshot, observed, utc);
        if (!policy->active.metrics) policy->active.id = 0;
    }
    while (i < policy->count) {
        NotificationEvent *event = &policy->queue[i];
        reconcile_event(policy, event, snapshot, observed, utc);
        if (event->metrics) ++i;
        else remove_queued(policy, i);
    }
}

/* 관측 세션의 큐·추적·active를 비운다. 통계·다음 ID·요청 간격과 진행 중 dispatch_id는
 * 남겨 이전 요청의 결과를 대응시킬 수 있게 한다. 관측 상태는 새로 시작한다. */
void notification_policy_reset(NotificationPolicy *policy)
{
    policy->count = policy->track_count = policy->program_count = policy->session = 0;
    policy->last_sequence = policy->last_observation_ms = 0;
    policy->active.id = 0;
    policy->active_sent = policy->dispatch_valid = policy->growth_armed = 0;
}

void notification_policy_enable(NotificationPolicy *policy, int enabled)
{
    if (policy->enabled == enabled) return;
    notification_policy_reset(policy);
    policy->enabled = enabled;
}

static void clear_growth(NotificationTrack *track, unsigned metric)
{
    track->growth_count[metric] = 0;
    track->growth_end[metric] = track->candidate_since[metric] = 0;
}

static void recover_metric(NotificationPolicy *policy, NotificationTrack *track, unsigned metric)
{
    unsigned bit = 1u << metric;
    if (track->deferred & bit) {
        NotificationProgram *program = program_record(policy, track->program, 0);
        if (program && program->ended[metric] < UINT_MAX) ++program->ended[metric];
        ++policy->stats.suppressed_ended;
    }
    track->accepted &= ~bit;
    track->provisional &= ~bit;
    track->deferred &= ~bit;
    track->retry_at[metric] = 0;
}

/* 오래된 관측은 미전달 큐와 증가 확인을 무효화한다. 이미 요청한 Shell 결과를
 * 맞춰 받을 dispatch_id와 수락 이력까지 새 세션처럼 지우지는 않는다. */
static void invalidate_stale_observation(NotificationPolicy *policy)
{
    policy->stats.invalidated += policy->count;
    policy->count = 0;
    if (policy->active.id && !policy->active_sent) {
        ++policy->stats.invalidated;
        policy->active.id = 0;
    }
    policy->dispatch_valid = policy->growth_armed = 0;
    for (unsigned pos = 0; pos < policy->track_count; ++pos) {
        policy->tracks[pos].provisional = 0;
        for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric)
            clear_growth(&policy->tracks[pos], metric);
    }
}

/* 신규 track 삽입으로 배열 위치가 달라질 수 있으므로 삽입을 먼저 끝낸다.
 * 그 뒤 같은 PID의 부재·유일 행·중복 행을 고정된 observed 인덱스로 구분한다.
 * completed_windows가 없는 일반 표본은 새 후보 track을 만들지 않는다. */
static void index_observed_tracks(NotificationPolicy *policy, const MonitorSnapshot *snapshot,
    size_t *observed, uint64_t utc)
{
    size_t i;
    unsigned pos;
    if (snapshot->completed_windows) {
        for (i = 0; i < snapshot->count; ++i) {
            const MonitorRow *row = &snapshot->rows[i];
            if (known_row(row) && recent_row(row, utc) && !suppressed(row, utc) && candidate_metrics(row, 1))
                add_track(policy, row);
        }
    }
    for (pos = 0; pos < policy->track_count; ++pos) observed[pos] = ROW_ABSENT;
    for (i = 0; i < snapshot->count && policy->track_count; ++i) {
        const MonitorRow *row = &snapshot->rows[i];
        pos = track_position(policy, row->raw.pid);
        if (pos < policy->track_count && policy->tracks[pos].pid == row->raw.pid)
            observed[pos] = observed[pos] == ROW_ABSENT ? i : ROW_AMBIGUOUS;
    }
}

/* 완료창이 없는 표본도 결측·억제·신원 실패로 증가 확인을 끊을 수 있다.
 * 따라서 이 검사는 완료창 전용 분기보다 먼저 실행한다. 정상 회복 판정은 하지 않는다. */
static void reconcile_confirmation(NotificationPolicy *policy, const MonitorSnapshot *snapshot,
    const size_t *observed, uint64_t utc)
{
    if (policy->growth_armed || policy->dispatch_valid) {
        policy->growth_armed = 0;
        for (unsigned pos = 0; pos < policy->track_count; ++pos) {
            NotificationTrack *track = &policy->tracks[pos];
            const MonitorRow *row = current_track_row(policy, track, snapshot, observed, utc);
            unsigned live = row && !suppressed(row, utc) ? candidate_metrics(row, 0) : 0;
            if (!row || suppressed(row, utc)) track->provisional = 0;
            for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
                if (!(live & (1u << metric))) clear_growth(track, metric);
                if (track->growth_end[metric]) policy->growth_armed = 1;
            }
        }
    }
}

/* 이번에 끝난 정상창만 회복으로 인정하고 새 후보를 큐에 넣는다. 관측된 신원 불명·
 * 중복 PID track은 보존하며 부재 track만 제거한다. 같은 PID라도 생성시각·프로그램이
 * 바뀌면 슬롯을 새 인스턴스로 초기화한다. observed는 압축 전 위치이므로 현재 위치를
 * 다 처리한 뒤 앞쪽 kept 슬롯에 복사한다. */
static void observe_completed_windows(NotificationPolicy *policy, const MonitorSnapshot *snapshot,
    const size_t *observed, uint64_t now, uint64_t utc)
{
    unsigned pos, kept;
    for (pos = 0, kept = 0; pos < policy->track_count; ++pos) {
        NotificationTrack *track = &policy->tracks[pos];
        if (observed[pos] == ROW_ABSENT) continue;
        if (observed[pos] != ROW_AMBIGUOUS) {
            const MonitorRow *row = &snapshot->rows[observed[pos]];
            if (known_row(row) && recent_row(row, utc)) {
                unsigned metric;
                if (track->creation != row->raw.creation_time_utc_100ns || track->program != row->program_id) {
                    *track = (NotificationTrack){.pid = row->raw.pid,
                        .creation = row->raw.creation_time_utc_100ns, .program = row->program_id};
                }
                for (metric = 0; metric < MONITOR_METRICS; ++metric) {
                    const MonitorMetricReport *report = &row->metric[metric];
                    if (report->state == MONITOR_NORMAL && metric_ready(row, metric) &&
                        report->window_end_utc_100ns == row->raw.measured_at_utc_100ns)
                        recover_metric(policy, track, metric);
                }
                if (!suppressed(row, utc))
                    enqueue_candidate(policy, track, row, candidate_metrics(row, 1), now);
            }
        }
        policy->tracks[kept++] = *track;
    }
    policy->track_count = kept;
}

/* snapshot은 호출 동안만 빌린다. 세션·회차·신선도를 통과한 전체 관측으로 미전달
 * 근거와 연속성을 먼저 검증한 뒤, 완료창이 있을 때만 회복·후보 상태를 전이한다. */
void notification_policy_observe(NotificationPolicy *policy, const MonitorSnapshot *snapshot, unsigned session,
    uint64_t now, uint64_t utc)
{
    size_t observed[NOTIFICATION_TRACK_LIMIT];
    if (!policy->enabled) return;
    if (!snapshot || snapshot->storage_error || snapshot->collector_error || snapshot->synthetic) {
        notification_policy_reset(policy);
        return;
    }
    if (policy->session != session) {
        notification_policy_reset(policy);
        policy->session = session;
    }
    if (!snapshot->sequence || snapshot->sequence <= policy->last_sequence) return;
    policy->last_sequence = snapshot->sequence;
    if (!snapshot->measured_utc_100ns || !recent(utc, snapshot->measured_utc_100ns,
        NOTIFICATION_FRESH_MS * (MONITOR_TICKS_PER_SECOND / 1000))) {
        invalidate_stale_observation(policy);
        return;
    }
    policy->last_observation_ms = now;
    expire_queued(policy, now);
    if (!snapshot->completed_windows && !policy->count && (!policy->active.id || policy->active_sent) &&
        !policy->growth_armed && !policy->dispatch_valid) return;

    index_observed_tracks(policy, snapshot, observed, utc);
    reconcile_pending(policy, snapshot, observed, utc);
    reconcile_confirmation(policy, snapshot, observed, utc);
    if (snapshot->completed_windows)
        observe_completed_windows(policy, snapshot, observed, now, utc);
}

void notification_policy_feedback(NotificationPolicy *policy, int64_t program, MonitorFeedback kind,
    uint64_t expires, uint64_t utc)
{
    unsigned i = 0;
    if (kind == MONITOR_FEEDBACK_CANCEL || expires <= utc) return;
    while (i < policy->count) {
        if (policy->queue[i].program == program) {
            ++policy->stats.feedback_purged;
            remove_queued(policy, i);
        } else ++i;
    }
    if (policy->active.id && policy->active.program == program) {
        ++policy->stats.feedback_purged;
        policy->active.id = 0;
    }
    if (policy->dispatch_valid && policy->active.program == program) {
        for (unsigned member = 0; member < policy->active.member_count; ++member) {
            NotificationTrack *track = member_track(policy, &policy->active, &policy->active.members[member]);
            if (track) track->provisional &= ~policy->active.members[member].metrics;
        }
        policy->dispatch_valid = 0;
    }
}

int notification_policy_begin(NotificationPolicy *policy, uint64_t now, int64_t feedback_in_progress,
    NotificationEvent *out)
{
    if (!policy->enabled || !policy->count) return 0;
    expire_queued(policy, now);
    if (!policy->count || policy->active.id || policy->dispatch_id || now < policy->next_request_ms ||
        now < after(policy->queue[0].queued_ms, NOTIFICATION_JOIN_MS) ||
        !recent(now, policy->last_observation_ms, NOTIFICATION_FRESH_MS) ||
        (feedback_in_progress && policy->queue[0].program == feedback_in_progress)) return 0;
    policy->active = policy->queue[0];
    policy->active_sent = 0;
    remove_queued(policy, 0);
    *out = policy->active;
    return 1;
}

static unsigned dispatch_metrics(NotificationPolicy *policy, NotificationTrack *track,
    const NotificationProgram *program, unsigned mask, const NotificationReason *reason, uint64_t now)
{
    if (!track) return 0;
    for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
        unsigned bit = 1u << metric;
        if ((mask & bit) && (reason[metric] == NOTIFICATION_REASON_RECURRENCE ||
            reason[metric] == NOTIFICATION_REASON_DEFERRED) && recurrence_quiet(program, metric, now)) {
            mark_deferred(policy, track, bit);
            mask &= ~bit;
        }
    }
    return mask;
}

static void recheck_recurrences(NotificationPolicy *policy, NotificationEvent *event,
    const NotificationProgram *program, uint64_t now)
{
    unsigned kept = 0;
    event->metrics = 0;
    for (unsigned i = 0; i < event->member_count; ++i) {
        NotificationMember *member = &event->members[i];
        NotificationTrack *track = member_track(policy, event, member);
        member->metrics = dispatch_metrics(policy, track, program, member->metrics, member->reason, now);
        if (member->metrics) {
            event->metrics |= member->metrics;
            event->members[kept++] = *member;
        }
    }
    event->member_count = kept;
    if (!event->omitted_count) return;
    event->omitted_count = event->omitted_metrics = 0;
    for (unsigned i = 0; i < policy->track_count; ++i) {
        NotificationTrack *track = &policy->tracks[i];
        if (track->overflow_id != event->id) continue;
        track->overflow_metrics = dispatch_metrics(policy, track, program, track->overflow_metrics, track->overflow_reason, now);
        if (track->overflow_metrics) {
            ++event->omitted_count;
            event->omitted_metrics |= track->overflow_metrics;
        } else track->overflow_id = 0;
    }
}

/* Shell 호출 직전 active를 재검증한다. 그 사이 피드백 저장이 시작됐으면 큐로 돌리고,
 * 만료·근거 소실이면 폐기한다. 성공 뒤 active 근거는 고정하며 result가 수락/실패를 확정한다. */
int notification_policy_dispatch(NotificationPolicy *policy, uint64_t id, uint64_t now,
    int64_t feedback_in_progress, NotificationEvent *out)
{
    NotificationProgram *program;
    unsigned summarized = 0;
    if (!policy->enabled || !id || policy->active.id != id || policy->active_sent || policy->dispatch_id) return 0;
    if (!recent(now, policy->active.queued_ms, NOTIFICATION_TTL_MS - 1)) {
        retry_event(policy, &policy->active, now);
        ++policy->stats.expired;
        policy->active.id = 0;
        return 0;
    }
    if (!policy->active.member_count || !recent(now, policy->last_observation_ms, NOTIFICATION_FRESH_MS)) {
        ++policy->stats.invalidated;
        policy->active.id = 0;
        return 0;
    }
    if (feedback_in_progress == policy->active.program) {
        if (policy->count == NOTIFICATION_QUEUE_LIMIT) {
            retry_event(policy, &policy->active, now);
            ++policy->stats.overflow;
        } else {
            memmove(&policy->queue[1], &policy->queue[0], policy->count * sizeof(policy->queue[0]));
            policy->queue[0] = policy->active;
            ++policy->count;
        }
        policy->active.id = 0;
        return 0;
    }
    program = recurrence_ms ? program_record(policy, policy->active.program, 0) : NULL;
    recheck_recurrences(policy, &policy->active, program, now);
    if (!policy->active.member_count) {
        policy->active.id = 0;
        return 0;
    }
    for (unsigned i = 0; i < policy->active.member_count; ++i) {
        NotificationMember *member = &policy->active.members[i];
        NotificationTrack *track = member_track(policy, &policy->active, member);
        if (track) track->provisional |= member->metrics;
        if (!program) continue;
        for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
            unsigned bit = 1u << metric;
            if (!(member->metrics & bit) || (summarized & bit)) continue;
            member->recurrence_count[metric] = program->ended[metric] < UINT16_MAX ? program->ended[metric] : UINT16_MAX;
            summarized |= bit;
        }
    }
    retry_omitted(policy, &policy->active, now);
    policy->stats.omitted += policy->active.omitted_count;
    policy->dispatch_id = id;
    policy->dispatch_valid = 1;
    policy->active_sent = 1;
    policy->next_request_ms = after(now, NOTIFICATION_SPACING_MS);
    ++policy->stats.requests;
    *out = policy->active;
    return 1;
}

void notification_policy_result(NotificationPolicy *policy, const NotificationEvent *event, int accepted, uint64_t now)
{
    NotificationProgram *program = NULL;
    unsigned advanced = 0;
    if (!event->id || event->id != policy->dispatch_id) return;
    policy->dispatch_id = 0;
    if (accepted) ++policy->stats.accepted;
    else ++policy->stats.failed;
    if (policy->dispatch_valid) for (unsigned i = 0; i < event->member_count; ++i) {
        const NotificationMember *member = &event->members[i];
        NotificationTrack *track = member_track(policy, event, member);
        unsigned valid = track ? track->provisional & member->metrics : 0;
        if (!valid) continue;
        track->provisional &= ~valid;
        if (!accepted) {
            retry_metrics(track, valid, now);
            continue;
        }
        track->accepted |= valid;
        track->delivered |= valid;
        track->deferred &= ~valid;
        for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
            unsigned bit = 1u << metric;
            if (!(valid & bit)) continue;
            track->reference[metric] = member->metric[metric].current;
            clear_growth(track, metric);
            if (!recurrence_ms) continue;
            if (!program) program = program_record(policy, event->program, 1);
            program->accepted_at[metric] = now;
            program->accepted_metrics |= bit;
            advanced |= bit;
        }
    }
    if (program) for (unsigned i = 0; i < event->member_count; ++i) {
        for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric) {
            unsigned count = event->members[i].recurrence_count[metric];
            if (advanced & (1u << metric))
                program->ended[metric] -= count < program->ended[metric] ? count : program->ended[metric];
        }
    }
    policy->dispatch_valid = 0;
    if (!accepted) notification_policy_retire(policy, event->id);
}

void notification_policy_retire(NotificationPolicy *policy, uint64_t id)
{
    if (policy->active.id == id) policy->active.id = 0;
}

int notification_policy_click(NotificationPolicy *policy, uint64_t id, uint64_t now, NotificationEvent *out)
{
    if (!id || policy->active.id != id || !policy->active_sent) return 0;
    *out = policy->active;
    policy->active.id = 0;
    return recent(now, out->queued_ms, NOTIFICATION_TTL_MS - 1);
}

unsigned notification_cause(const NotificationEvent *event)
{
    return event->member_count ? notification_member_cause(&event->members[0]) : MONITOR_METRICS;
}

unsigned notification_member_cause(const NotificationMember *member)
{
    for (unsigned metric = 0; metric < MONITOR_METRICS; ++metric)
        if (member->metrics & (1u << metric)) return metric;
    return MONITOR_METRICS;
}
