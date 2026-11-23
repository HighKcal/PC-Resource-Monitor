/* 품질을 충족한 현재 창을 과거 분포와 비교한다. monitor_baseline에서 과거 선택→통계→판정→
 * 결과 공개 순서로 읽는다. 숫자 규칙은 monitor_policy.h, 학습 포함·후보·다음 streak의 선택은
 * decide_completed_metric이 맡는다. DB 쓰기와 인스턴스 상태 적용은 호출자가 담당한다. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include "baseline.h"
/* 상한 최소 증가폭의 단위: CPU 퍼센트포인트, WorkingSet 바이트, 읽기·쓰기 B/s. */
static const double metric_threshold_floors[4] = {
    BASELINE_CPU_FLOOR, BASELINE_RAM_FLOOR, BASELINE_READ_FLOOR, BASELINE_WRITE_FLOOR };
typedef struct BaselineDecision {
    MonitorState state;
    int eligible;
    unsigned next_streak;
    const char *reason;
} BaselineDecision;

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
/* 정밀 시작시각보다 엄격히 앞서고 회차 cutoff 이하인 이력만 수집한다. 각 조회의 임시 배열과
 * values는 이 호출의 스택에만 있다. 실패하면 부분 표본은 버리고 외부 report를 건드리지 않는다. */
static int select_past_values(BaselineHistoryScope *history, const BaselineInput *input,
    double values[BASELINE_MAX_SAMPLES], unsigned *sample_count, unsigned *date_count)
{
    unsigned days_ago;
    uint64_t utc_date = input->exact_start / MONITOR_DAY_TICKS;
    *sample_count = *date_count = 0;
    for (days_ago = 0; days_ago < BASELINE_HISTORY_UTC_DAYS && utc_date >= days_ago; ++days_ago) {
        double selected[BASELINE_SAMPLES_PER_DAY];
        uint64_t lower = (utc_date - days_ago) * MONITOR_DAY_TICKS, upper = lower + MONITOR_DAY_TICKS;
        unsigned previous_count = *sample_count, selected_count, i;
        if (!baseline_history_select_day(history, input->program_id, input->metric,
            lower, upper, input->exact_start, selected, &selected_count)) return 0;
        for (i = 0; i < selected_count && *sample_count < BASELINE_MAX_SAMPLES; ++i)
            values[(*sample_count)++] = selected[i];
        if (*sample_count > previous_count) ++(*date_count);
    }
    return 1;
}

/* 과거 값 배열은 정렬해도 되는 지역 사본이다. report의 현재값·품질·관측창은 바꾸지 않는다.
 * 이력이 없으면 통계값도 유지한다. 호출자는 창 품질 검사에서 이를 NAN으로 초기화해 둔다. */
static void summarize_history(double *values, unsigned sample_count, unsigned date_count,
    unsigned metric, MonitorMetricReport *r)
{
    double deviations[BASELINE_MAX_SAMPLES];
    unsigned index;
    /* 재귀 선택은 UTC 종료 분을 거슬러 내려가며 날짜 범위가 겹치지 않아 표본 수가 고유 분 수다. */
    r->count = sample_count;
    r->days = date_count;
    r->minutes = sample_count;

    /* 중앙값과 MAD를 구한 뒤 변동폭·중앙값 비율·최소 증가폭 중 최댓값을 중앙값에 더해 상한을 정한다. */
    if (sample_count) {
        r->median = median(values, sample_count);
        for (index = 0; index < sample_count; ++index)
            deviations[index] = fabs(values[index] - r->median);
        r->mad = median(deviations, sample_count);
        r->threshold = r->median + fmax(fmax(BASELINE_MAD_MARGIN_MULTIPLIER * BASELINE_MAD_NORMAL_SCALE * r->mad,
            BASELINE_MEDIAN_MARGIN_RATIO * r->median), metric_threshold_floors[metric]);
    }
}

/* 준비 전·준비 후 정상 창은 학습에 포함하고, 준비 후 상한 초과 창은 제외한다.
 * 현재값과 과거 통계만 읽어 다음 상태를 반환한다. prior_streak은 경로별 표본 수가 아니라
 * 이 인스턴스·지표의 연속 초과 횟수다. CANDIDATE는 알림 정책이 소비할 후보이며 발송 결정은 아니다. */
static BaselineDecision decide_completed_metric(const MonitorMetricReport *r, unsigned prior_streak)
{
    BaselineDecision decision = {0};
    if (r->count < BASELINE_READY_WINDOWS || r->days < BASELINE_READY_UTC_DAYS ||
        r->minutes < BASELINE_READY_MINUTES) {
        decision.state = MONITOR_LEARNING;
        decision.eligible = 1;
        decision.reason = "need " MONITOR_POLICY_TEXT(BASELINE_READY_WINDOWS) " windows / "
            MONITOR_POLICY_TEXT(BASELINE_READY_UTC_DAYS) " UTC dates / "
            MONITOR_POLICY_TEXT(BASELINE_READY_MINUTES) " distinct minutes";
    } else if (r->current > r->threshold) {
        decision.next_streak = prior_streak < UINT_MAX ? prior_streak + 1 : prior_streak;
        decision.state = decision.next_streak >= BASELINE_CANDIDATE_STREAK ? MONITOR_CANDIDATE : MONITOR_ATTENTION;
        decision.reason = "upper threshold exceeded; excluded from baseline";
    } else {
        decision.state = MONITOR_NORMAL;
        decision.eligible = 1;
        decision.reason = "ready metric at or below upper threshold";
    }
    return decision;
}

int monitor_baseline(BaselineHistoryScope *history, const BaselineInput *input,
    MonitorMetricReport *r, unsigned *next_streak)
{
    double values[BASELINE_MAX_SAMPLES];
    unsigned sample_count, date_count;
    BaselineDecision decision;
    if (!select_past_values(history, input, values, &sample_count, &date_count)) return 0;
    /* 실패 가능한 조회를 모두 마친 뒤 결과를 채운다. 아래 통계·판정에는 실패 경로가 없으므로
     * report 전체 사본이 필요 없다. 조회 실패에는 report/next_streak 모두 기존 값 그대로다. */
    summarize_history(values, sample_count, date_count, input->metric, r);
    decision = decide_completed_metric(r, input->prior_streak);
    r->state = decision.state;
    r->baseline_eligible = decision.eligible;
    r->streak = decision.next_streak;
    snprintf(r->reason, sizeof(r->reason), "%s", decision.reason);
    /* 호출자는 다음 횟수를 인스턴스에 적용하고 report의 eligible 값으로 현재 창을 저장한다. */
    *next_streak = decision.next_streak;
    return 1;
}
