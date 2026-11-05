/* 품질을 충족한 현재 창을 과거 분포와 비교한다. 현재 창의 학습 포함 여부도 여기서 정한다.
 * DB 쓰기와 인스턴스 연속 초과 상태 적용은 호출자가 담당한다. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include "baseline.h"
/* 상한 최소 증가폭의 단위: CPU 퍼센트포인트, WorkingSet 바이트, 읽기·쓰기 B/s. */
static const double metric_threshold_floors[4] = {
    BASELINE_CPU_FLOOR, BASELINE_RAM_FLOOR, BASELINE_READ_FLOOR, BASELINE_WRITE_FLOOR };

static int double_compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}


static double median(double *v, unsigned n)
{
    qsort(v, n, sizeof(*v), double_compare);
    return n % 2 ? v[n / 2] : v[n / 2 - 1] / 2 + v[n / 2] / 2;
}


static void compare_history(const BaselineInput *input, double *values,
    unsigned sample_count, unsigned date_count, MonitorMetricReport *r, unsigned *next_streak)
{
    double deviations[BASELINE_MAX_SAMPLES];
    unsigned distinct_minutes, index, metric = input->metric, streak = input->prior_streak;
    /* 재귀 선택은 UTC 종료 분을 거슬러 내려가며 날짜 범위가 겹치지 않아 표본 수가 고유 분 수다. */
    distinct_minutes = sample_count;
    r->count = sample_count;
    r->days = date_count;
    r->minutes = distinct_minutes;

    /* 중앙값과 MAD를 구한 뒤 변동폭·중앙값 비율·최소 증가폭 중 최댓값을 중앙값에 더해 상한을 정한다. */
    if (sample_count) {
        r->median = median(values, sample_count);
        for (index = 0; index < sample_count; ++index)
            deviations[index] = fabs(values[index] - r->median);
        r->mad = median(deviations, sample_count);
        r->threshold = r->median + fmax(fmax(3 * 1.4826 * r->mad, 0.5 * r->median), metric_threshold_floors[metric]);
    }

    /* 준비 전 창·준비 후 정상 창은 학습에 포함하고, 준비 후 상한 초과 창은 제외한다.
     * 연속 초과 횟수는 인스턴스별로 유지한다. */
    if (sample_count < BASELINE_READY_WINDOWS || date_count < BASELINE_READY_UTC_DAYS ||
        distinct_minutes < BASELINE_READY_MINUTES) {
        streak = 0;
        r->state = MONITOR_LEARNING;
        r->baseline_eligible = 1;
        snprintf(r->reason, sizeof(r->reason), "need 60 windows / 3 UTC dates / 60 distinct minutes");
    } else if (r->current > r->threshold) {
        if (streak < UINT_MAX) ++streak;
        r->state = streak >= BASELINE_CANDIDATE_STREAK ? MONITOR_CANDIDATE : MONITOR_ATTENTION;
        r->baseline_eligible = 0;
        snprintf(r->reason, sizeof(r->reason), "upper threshold exceeded; excluded from baseline");
    } else {
        streak = 0;
        r->state = MONITOR_NORMAL;
        r->baseline_eligible = 1;
        snprintf(r->reason, sizeof(r->reason), "ready metric at or below upper threshold");
    }
    r->streak = streak;
    *next_streak = streak;
}

int monitor_baseline(BaselineHistoryScope *history, const BaselineInput *input,
    MonitorMetricReport *r, unsigned *next_streak)
{
    double values[BASELINE_MAX_SAMPLES];
    unsigned sample_count = 0, date_count = 0, days_ago;
    uint64_t utc_date = input->exact_start / MONITOR_DAY_TICKS;
    /* 현재 창보다 앞선 이력을 먼저 모은 뒤 통계와 다음 연속 초과 횟수를 계산한다. */
    for (days_ago = 0; days_ago < BASELINE_HISTORY_UTC_DAYS && utc_date >= days_ago; ++days_ago) {
        double selected[BASELINE_SAMPLES_PER_DAY];
        uint64_t lower = (utc_date - days_ago) * MONITOR_DAY_TICKS, upper = lower + MONITOR_DAY_TICKS;
        unsigned previous_count = sample_count, selected_count, i;
        if (!baseline_history_select_day(history, input->program_id, input->metric, lower, upper, input->exact_start, selected, &selected_count)) return 0;
        for (i = 0; i < selected_count && sample_count < BASELINE_MAX_SAMPLES; ++i)
            values[sample_count++] = selected[i];
        if (sample_count > previous_count) ++date_count;
    }
    compare_history(input, values, sample_count, date_count, r, next_streak);
    return 1;
}
