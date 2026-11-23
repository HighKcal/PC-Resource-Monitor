using System;
using System.Collections.Generic;
using System.Linq;

namespace PcMonitor;

// UI 전용 값 복사본이다. UiRow의 현재 관측과 아래 세 종류의 비교 근거는 시점이 다르다.
// 수집·저장·그룹 정책·알림 수락을 실행하지 않으며 표시 우선순위는 MainWindow가 정한다.
public enum UiMetric { Cpu = 0, Ram = 1, Read = 2, Write = 3, Gpu = 4 }

// 마지막 완료 구간 Average와 그때 선택한 장기 기준 Median/Upper다. Start/End는
// 완료 구간 시각이며 현재 원시값의 관측 시각이 아니다. CPU는 %, RAM은 Working Set
// bytes, 읽기/쓰기는 B/s이고 null은 결측이다. 화면 단위 변환은 ViewFormat이 맡는다.
public sealed record UiComparison
{
    public UiMetric Metric { get; init; }
    // 최신 native 연속 판정이다. 표시 문구로 후보·학습 상태를 추측하지 않는다.
    public MetricState StateCode { get; init; }
    // 현재 연속 판정과 마지막 완료창은 시점이 다르다. 완료창 메타데이터가 있으면
    // 그 상태로 준비·학습을 설명하고, 현재 Candidate 여부는 StateCode만 사용한다.
    public MetricState? CompletedStateCode { get; init; }
    public MetricState WindowStateCode => CompletedStateCode ?? StateCode;
    private static string StateLabel(MetricState state) => state switch {
        MetricState.Learning => "개인 기준 학습 중", MetricState.Normal => "평소 범위",
        MetricState.Attention => "상한 초과", MetricState.Candidate => "변화 후보", _ => "비교 근거 없음"
    };
    public string State => StateLabel(StateCode);
    public string WindowState => StateLabel(WindowStateCode);
    public string Reason { get; init; } = "";
    public double? Average { get; init; }
    public double? Upper { get; init; }
    public double? Median { get; init; }
    public DateTimeOffset? Start { get; init; }
    public DateTimeOffset? End { get; init; }
    public bool Candidate => StateCode == MetricState.Candidate;
    public bool Ready => WindowStateCode >= MetricState.Normal;
    public string Evidence { get; init; } = "";
}

// 선택한 인스턴스의 설명용 근거다. 후보·알림·피드백 정책이나 목록 조회에 사용하지 않는다.
public enum RecentComparisonState { Waiting, Preparing, Ready, LongTerm }
// Average/Start/End는 최신 유효 완료 구간, Median/HistoryStart/HistoryEnd와 Count는
// 그 구간을 제외한 과거 유효 구간이다. 수치 단위는 UiComparison과 같고 Duration은
// 실제 구간 초다. 개인 기준 준비 여부와 지표별 최근 준비 상태를 별도로 보존한다.
public sealed record UiRecentComparison
{
    public UiMetric Metric { get; init; }
    public RecentComparisonState State { get; init; }
    public uint Count { get; init; }
    public double? Median { get; init; }
    public double? Average { get; init; }
    public DateTimeOffset? Start { get; init; }
    public DateTimeOffset? End { get; init; }
    public DateTimeOffset? HistoryStart { get; init; }
    public DateTimeOffset? HistoryEnd { get; init; }
    public double Duration { get; init; }
    public string Evidence { get; init; } = "";
}

// 알림에 복사된 지표의 당시 평균·상한·구간이다. Upper가 없어도 알림 근거 선택을
// 해제하거나 최신 상한으로 보충하지 않는다. 단위는 UiComparison과 같다. AcceptedAt은
// Shell 요청 수락 시각이며 관측 구간과 다르다. 클릭 복사본에서는 알 수 없어 null이다.
public sealed record UiAlert
{
    public UiMetric Metric { get; init; }
    public string Reason { get; init; } = "";
    public double? Average { get; init; }
    public double? Upper { get; init; }
    public DateTimeOffset? Start { get; init; }
    public DateTimeOffset? End { get; init; }
    public DateTimeOffset? AcceptedAt { get; init; }
    public string Evidence { get; init; } = "";
    internal bool HasWindowEvidence => Start.HasValue && End > Start &&
        Average is double average && double.IsFinite(average) && average >= 0 &&
        Upper is double upper && double.IsFinite(upper) && upper >= 0;
}

public sealed record UiRow
{
    public uint Pid { get; init; }
    public ulong Creation { get; init; }
    public bool IdentityValid { get; init; } = true;
    public string Key => $"{Pid}:{Creation}";
    public string GroupKey { get; init; } = "__other";
    public string Name { get; init; } = "이름 확인 불가";
    public string Path { get; init; } = "";
    public double? CpuPercent { get; init; }
    public double? GpuPercent { get; init; }
    public double? RamPercent { get; init; }
    public double? WorkingSetBytes { get; init; }
    public double? ReadBytesPerSecond { get; init; }
    public double? WriteBytesPerSecond { get; init; }
    public DateTimeOffset? ObservedAt { get; init; }
    public string Feedback { get; init; } = "";
    public string ProgramId { get; init; } = "";
    public string Evidence { get; init; } = "";
    public UiAlert? Alert { get; init; }
    public IReadOnlyList<UiAlert> Alerts { get; init; } = Array.Empty<UiAlert>();
    // worker가 보관한 지표별 최신 수락 원본과 UI에서 연 한 사건을 분리한다. 클릭 사건에
    // 없는 지표는 자체 수락 근거를 읽지만, 이번 클릭의 원인·목록 개수에는 더하지 않는다.
    internal IReadOnlyList<UiAlert> ClickedAlerts { get; init; } = Array.Empty<UiAlert>();
    internal IEnumerable<UiAlert> SavedAlerts => Alert == null ? Alerts : Alerts.Prepend(Alert);
    internal IEnumerable<UiAlert> AlertCauses => ClickedAlerts.Count != 0 ? ClickedAlerts : SavedAlerts;
    internal bool HasAlert => AlertCauses.Any();
    internal IEnumerable<UiMetric> AlertMetrics => AlertCauses.Select(a => a.Metric).Distinct();
    internal UiAlert? AlertEvidence(UiMetric metric) => ClickedAlerts.FirstOrDefault(a => a.Metric == metric) ??
        Alerts.FirstOrDefault(a => a.Metric == metric) ?? (Alert?.Metric == metric ? Alert : null);
    public IReadOnlyList<UiComparison> Comparisons { get; init; } = Array.Empty<UiComparison>();
    public IReadOnlyList<UiRecentComparison> RecentComparisons { get; init; } = Array.Empty<UiRecentComparison>();
}

public sealed record UiGroup
{
    public string Key { get; init; } = "__other";
    public string Name { get; init; } = "기타";
    public string Color { get; init; } = "#7C8892";
    public string Description { get; init; } = "";
    public double? CpuPercent { get; init; }
    public double? GpuPercent { get; init; }
    public double? RamPercent { get; init; }
    public double? WorkingSetBytes { get; init; }
    public double? ReadBytesPerSecond { get; init; }
    public double? WriteBytesPerSecond { get; init; }
    public int CpuValid { get; init; }
    public int GpuValid { get; init; }
    public int RamValid { get; init; }
    public int ReadValid { get; init; }
    public int WriteValid { get; init; }
    public int MemberCount { get; init; }
    public int AlertCount { get; init; }
    public string Evidence { get; init; } = "";
}

public sealed record UiSlice
{
    public string Key { get; init; } = "";
    public string Name { get; init; } = "";
    public string Color { get; init; } = "#7C8892";
    public double? Percent { get; init; }
    public double DrawPercent { get; init; }
    public int Valid { get; init; }
    public int Total { get; init; }
    public bool Free { get; init; }
    public bool Unattributed { get; init; }
    internal string DisplayPercent => ViewFormat.WithCoverage(ViewFormat.Percent(Percent), Valid, Total);
}

public sealed record UiChart
{
    public string MetricName { get; init; } = "CPU";
    public double? Total { get; init; }
    public string Explanation { get; init; } = "";
    public string Note { get; init; } = "";
    // false이면 라벨에는 관측 비율을 남기고 도넛에는 시스템 사용/여유만 그린다.
    // 서로 겹치는 프로세스 합계는 전체를 분할하는 원형 그래프로 표시하지 않는다.
    public bool CanPartition { get; init; } = true;
    public IReadOnlyList<UiSlice> Slices { get; init; } = Array.Empty<UiSlice>();
}

public sealed record UiSnapshot
{
    public long SessionEpoch { get; init; }
    public long Sequence { get; init; }
    public DateTimeOffset? ObservedAt { get; init; }
    public bool Paused { get; init; }
    public bool IsSynthetic { get; init; }
    public bool NotificationsEnabled { get; init; } = true;
    public string Status { get; init; } = "첫 표본을 기다리는 중";
    public bool RetentionPending { get; init; }
    // 세션의 실제 native 그룹 갱신 상태다. 타이머·검색·정렬은 이 값을 바꾸지 않는다.
    public bool GroupRefreshPending { get; init; }
    public string CollectionStatus => RetentionPending ? Status + " · 오래된 기록 정리 지연" : Status;
    public string CommandStatus { get; init; } = "";
    public string Footer { get; init; } = "결측은 0으로 계산하지 않습니다.";
    public IReadOnlyList<UiRow> Rows { get; init; } = Array.Empty<UiRow>();
    public IReadOnlyList<UiGroup> Groups { get; init; } = Array.Empty<UiGroup>();
    public UiChart Cpu { get; init; } = new() { MetricName = "CPU" };
    public UiChart Gpu { get; init; } = new() { MetricName = "GPU" };
    public UiChart Ram { get; init; } = new() { MetricName = "RAM" };
}

public sealed record UiCommand(string Name, string? ProcessKey = null,
    string? GroupKey = null, UiMetric Metric = UiMetric.Ram, string? Argument = null);

// 표시 행의 임시 키는 신원이 아니다. native 복사본과 별개로 들어오는 합성/재표시
// 입력도 PID가 유일하고 생성시각이 확인된 실제 행만 명령·비동기 근거에 연결한다.
internal static class UiIdentity
{
    internal static bool IsVerified(UiSnapshot snapshot, UiRow? row) => row is { IdentityValid: true, Pid: > 0, Creation: > 0 } &&
        snapshot.Rows.Count(r => r.Pid == row.Pid) == 1 && snapshot.Rows.Any(r => ReferenceEquals(r, row));

    internal static UiRow? FindVerified(UiSnapshot snapshot, string key)
    {
        var row = snapshot.Rows.FirstOrDefault(r => r.Key == key);
        return IsVerified(snapshot, row) ? row : null;
    }
}
