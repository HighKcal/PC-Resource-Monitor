#ifndef MONITOR_EXPERIMENT_H
#define MONITOR_EXPERIMENT_H
#include "monitor.h"
#ifdef __cplusplus
extern "C" {
#endif

#define EXPERIMENT_POLICY_VERSION "change-events-v1"
#define EXPERIMENT_TEMPLATE_VERSION "local-evidence-v1"
#define EXPERIMENT_RETENTION_DAYS 28
typedef struct Experiment Experiment;
typedef enum ExperimentMethod { EXPERIMENT_PERSONALIZED, EXPERIMENT_FIXED } ExperimentMethod;
typedef enum ExperimentEpisodeState { EXPERIMENT_ACTIVE, EXPERIMENT_RECOVERED, EXPERIMENT_UNKNOWN } ExperimentEpisodeState;
typedef enum ExperimentJudgment { EXPERIMENT_UNRATED, EXPERIMENT_USEFUL, EXPERIMENT_KNOWN,
    EXPERIMENT_UNNECESSARY, EXPERIMENT_UNSURE } ExperimentJudgment;

typedef struct ExperimentDecision {
    int64_t id, window_id, source_window_id, source_run_id, source_cutoff, episode_id;
    uint64_t session, creation, start_utc, end_utc;
    uint32_t pid;
    unsigned metric;
    ExperimentMethod method;
    MonitorMetricReport report; /* 복사한 근거. 고정 규칙도 같은 현재값·품질·완료창을 사용한다. */
    int continuity, quality_valid;
    char *name, *path;
} ExperimentDecision;
typedef struct ExperimentEpisode {
    int64_t id, activation_decision_id, last_decision_id;
    uint64_t session, creation, first_utc, activated_utc, last_utc, ended_utc;
    uint32_t pid;
    unsigned metric, qualifying_windows;
    ExperimentMethod method;
    ExperimentEpisodeState state;
    char *name, *path, *end_reason;
} ExperimentEpisode;
typedef struct ExperimentEvaluation {
    int64_t episode_id; /* 0이면 사건 후보가 아닌 결정·전체창 보기에서도 평가할 수 있다. */
    int64_t decision_id;
    const char *trial_id, *order_id;
    ExperimentJudgment judgment;
    int understood, target_correct, time_correct, metric_correct, next_check; /* -1 모름, 0 아니오, 1 예 */
    double elapsed_seconds; /* 음수는 미측정이다. 시간을 추정해 채우지 않는다. */
    const char *note;
} ExperimentEvaluation;
typedef struct ExperimentStats {
    uint64_t windows, decisions, episodes, active, recovered, unknown, evaluations, gaps;
    uint64_t states[2][5];
    double window_seconds[2][5];
    int source_available, replay_only, synthetic, replay_comparator_recomputed;
    /* 보존된 비교창과 별개인 짝 원본 DB의 최근 회차 진단이다. 항목별 수는 겹칠 수 있다.
     * 지표 결측은 유효 비트 없음 또는 SQL NULL이다. 예상 raw 행을 모두 읽어야 조회 가능하다.
     * 원본이 없거나 만료돼 조회 불가하면 결측 수 0을 결측 없음으로 해석하지 않는다. */
    uint64_t latest_source_utc, latest_source_rows, latest_source_raw_rows;
    uint64_t latest_source_identity_unavailable, latest_source_path_unavailable;
    uint64_t latest_source_metric_missing[MONITOR_METRICS];
    int latest_source_available, latest_source_status;
} ExperimentStats;

/* 명시한 prototype 전용 파일만 사용한다. create는 두 파일이 모두 없어야 하며,
 * resume은 원본 DB의 실험 짝을 검증한 뒤 core 쓰기 connection을 연다.
 * 기존 일반 monitor DB는 거부하고 변환하지 않는다. worker thread에서만 호출한다. */
int experiment_create(const char *source_path, const char *evidence_path, int synthetic,
    Experiment **out, char *error, size_t error_size);
int experiment_resume(const char *evidence_path, Experiment **out, char *error, size_t error_size);
/* 독립 replay 결과를 포함한 저장 이력을 읽기 전용으로 연다. */
int experiment_open_history(const char *evidence_path, Experiment **out, char *error, size_t error_size);
/* 실험 DB의 검토·평가 쓰기만 허용하고 원본 DB는 쓰기용으로 열지 않는다. */
int experiment_open_review(const char *evidence_path, Experiment **out, char *error, size_t error_size);
/* 실제·합성 batch 한 회차를 처리한다. 성공 snapshot은 monitor_ingest와 같이
 * 호출자가 소유하고 monitor_snapshot_free로 해제한다. core가 먼저 commit되며,
 * 이후 실패는 명시적 부분 실패로 반환하고 reset/resume이 필요하다.
 * 이미 commit된 core 회차를 실험 DB에도 모두 기록한 것으로 취급하지 않는다. */
int experiment_ingest(Experiment *, const ResourceBatch *, const char *const *synthetic_paths,
    MonitorSnapshot **out);
int experiment_reset(Experiment *, uint64_t utc, const char *reason);
int experiment_finish(Experiment *, uint64_t utc); /* 활성 사건은 관측 중단으로 판단 불가 상태가 된다. */
const char *experiment_error(const Experiment *);
void experiment_close(Experiment *);
/* 호출자가 배열과 내부 문자열을 소유하고 해당 free 함수로 함께 해제한다.
 * 최신순이며 limit은 1..10000, offset은 0 이상이다. 두 방식은 같은 행·완료창 근거를 쓰며
 * 결정 목록에는 후보 이외의 모든 상태도 포함한다. */
int experiment_decisions(Experiment *, ExperimentMethod, unsigned limit, unsigned offset,
    ExperimentDecision **out, size_t *count);
int experiment_get_decision(Experiment *, int64_t id, ExperimentDecision **out);
void experiment_decisions_free(ExperimentDecision *, size_t);
int experiment_episodes(Experiment *, int method_or_minus_one, unsigned limit, unsigned offset,
    ExperimentEpisode **out, size_t *count);
void experiment_episodes_free(ExperimentEpisode *, size_t);
int experiment_stats(Experiment *, ExperimentStats *);
int experiment_evaluate(Experiment *, const ExperimentEvaluation *);
int experiment_export(Experiment *, const char *csv_path);
/* 기록된 불변 완료창·관측 단절 순서로 결정과 사건을 새 replay 표시 DB에 구성한다.
 * 앞부분이 온전하면 고정 규칙을 재계산해 원래 결과와 대조한다. 보관 정책으로 만료됐다면
 * 기록된 결정과 경계 요약을 사용하는 projection-only 방식이다. Baseline은 재학습하지 않는다.
 * 대상 파일은 없어야 하며 원본 실험 DB는 읽기 전용으로 연다. */
int experiment_replay(const char *evidence_path, const char *new_path, char *error, size_t error_size);
const char *experiment_method_name(ExperimentMethod);
const char *experiment_episode_state_name(ExperimentEpisodeState);
const char *experiment_judgment_name(ExperimentJudgment);
/* AI/API 없이 보존된 근거로 로컬 설명을 만든다. now로 오래된 완료창을 구분한다. */
int experiment_explain(const ExperimentDecision *, uint64_t now, char *text, size_t size);
#ifdef __cplusplus
}
#endif
#endif
