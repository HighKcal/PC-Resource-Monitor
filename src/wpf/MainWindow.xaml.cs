// 한 회차의 UiSnapshot을 목록·그룹·선택 상세에 표시하는 창이다. 수집·저장·판정은 여기서 실행하지 않는다.
// 개별 상세의 읽기 시작점은 RenderAnalysis → SelectAnalysisEvidence다. 먼저 같은 지표의 근거를
// 하나 선택한 뒤 해당 분기만 그리며, 현재 표본은 비교 구간과 분리한다. 입력은 읽기 전용으로 참조한다.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace PcMonitor;

public partial class MainWindow : Window
{
    public event Action<UiCommand>? CommandRequested;
    public event Action<UiRow?>? ProcessSelectionChanged;
    public UiSnapshot CurrentSnapshot { get; private set; } = new();
    public UiRow? SelectedRow
    {
        get
        {
            if (_selectedProcessKey == null) return null;
            if (_selectedIdentityVerified) return UiIdentity.FindVerified(CurrentSnapshot, _selectedProcessKey);
            if (CurrentSnapshot.SessionEpoch != _selectedRoundSession || CurrentSnapshot.Sequence != _selectedRoundSequence) return null;
            // 모호한 두 행은 같은 회차의 실제 선택 객체만 읽는다. 이름·순서·표시 슬롯으로
            // 다른 행을 추측하지 않으며, 단일 미확인 행의 기존 같은 회차 재표시는 유지한다.
            if (CurrentSnapshot.Rows.Any(r => ReferenceEquals(r, _selectedRow))) return _selectedRow;
            return _selectedRowWasUnique && CurrentSnapshot.Rows.Count(r => r.Key == _selectedProcessKey) == 1
                ? CurrentSnapshot.Rows.First(r => r.Key == _selectedProcessKey) : null;
        }
    }
    public UiGroup? SelectedGroup => _selectedGroupKey == null || (_selectedProcessKey != null && SelectedRow == null)
        ? null : CurrentSnapshot.Groups.FirstOrDefault(g => g.Key == _selectedGroupKey);
    public UiMetric SelectedMetric { get; private set; } = UiMetric.Ram;
    public string? SelectedProcessKey => _selectedProcessKey;
    public static readonly DependencyProperty IoColumnWidthProperty = DependencyProperty.Register(nameof(IoColumnWidth), typeof(GridLength), typeof(MainWindow), new PropertyMetadata(new GridLength(0)));
    public GridLength IoColumnWidth { get => (GridLength)GetValue(IoColumnWidthProperty); private set => SetValue(IoColumnWidthProperty, value); }
    private string? _selectedProcessKey;
    private string? _selectedGroupKey;
    private bool _selectedIdentityVerified;
    private UiRow? _selectedRow;
    private bool _selectedRowWasUnique;
    private long _selectedRoundSession;
    private long _selectedRoundSequence;
    private readonly ObservableCollection<ViewListRow> _visibleRows = new();
    private readonly Dictionary<string, ViewListRow> _rowCache = new();
    private readonly Dictionary<string, bool> _expanded = new();
    private bool _rendering;
    private bool _sidebarVisible = true;
    private bool _narrowDetail;
    private bool _grouped = true;
    private bool _hasSnapshot;
    private string _evidence = "";
    private string _commandStatus = "";
    private bool _commandStatusIsGroup;

    public MainWindow()
    {
        InitializeComponent();
        InitializeSmallGroupNavigation();
        InitializeWorkArea();
        ProcessList.ItemsSource = _visibleRows;
        Loaded += (_, _) => { FitWorkArea(); ApplyLayout(); };
        RenderSnapshot(CurrentSnapshot);
    }

    // WindowController가 발행한 회차를 UI 스레드에서 통째로 교체한다. 세션이 바뀌면
    // 선택을 해제하고, 신원이 불확실한 행의 선택은 다음 회차로 이어 붙이지 않는다.
    public void RenderSnapshot(UiSnapshot snapshot)
    {
        if (!Dispatcher.CheckAccess()) { Dispatcher.Invoke(() => RenderSnapshot(snapshot)); return; }
        bool newSession = _hasSnapshot && snapshot.SessionEpoch != CurrentSnapshot.SessionEpoch;
        bool newRound = newSession || snapshot.Sequence != CurrentSnapshot.Sequence;
        if (newSession || (_selectedProcessKey != null && !_selectedIdentityVerified && newRound))
        {
            ClearProcessSelection();
            if (newSession) _expanded.Clear();
            ProcessSelectionChanged?.Invoke(null);
        }
        // 호출자가 함께 복사한 한 회차를 사용한다. UI에서 서로 다른 native 회차의
        // 필드를 섞으면 현재 표본과 근거의 연결을 잃으므로 부분 병합하지 않는다.
        CurrentSnapshot = snapshot; _hasSnapshot = true;
        // 그룹 이동 대상과 안내는 native 세션과 수명이 같다. 수집/저장·피드백 안내는
        // 별도 출처이므로 새 세션이라는 이유만으로 지우지 않는다.
        if (newSession && _commandStatusIsGroup) { _commandStatus = ""; _commandStatusIsGroup = false; }
        RevalidateSmallGroups(snapshot);
        if (_selectedProcessKey != null && SelectedRow == null)
        {
            // 중복 PID로 모호해진 선택은 폐기한다. 통상적인 종료/일시 부재는 기존 key와
            // '현재 표본에서 확인할 수 없음' 안내를 유지하며 재사용 PID의 다른 생성시각으로 옮기지 않는다.
            if (_selectedRow != null && snapshot.Rows.Count(r => r.Pid == _selectedRow.Pid) > 1) ClearProcessSelection();
            ProcessSelectionChanged?.Invoke(null);
        }
        if (SelectedRow is UiRow row) _selectedGroupKey = row.GroupKey;
        if (_selectedGroupKey != null && SelectedGroup == null) _selectedGroupKey = null;
        MeasuredTime.Text = snapshot.ObservedAt.HasValue ? ViewFormat.Time(snapshot.ObservedAt) + " 측정" : "측정 전";
        DataMode.Text = snapshot.IsSynthetic ? "합성 데이터" : snapshot.Paused ? "일시정지" : "실제 PC 데이터";
        CollectionStatus.Text = snapshot.CollectionStatus;
        PauseButton.Content = snapshot.Paused ? "재개" : "일시정지";
        ProcessCount.Text = $"프로세스 {snapshot.Rows.Count:N0}개";
        CpuTotal.Text = ViewFormat.Percent(snapshot.Cpu.Total); GpuTotal.Text = ViewFormat.Percent(snapshot.Gpu.Total); RamTotal.Text = ViewFormat.Percent(snapshot.Ram.Total);
        ListFootnote.Text = snapshot.Footer;
        if (!string.IsNullOrWhiteSpace(snapshot.CommandStatus)) { _commandStatus = snapshot.CommandStatus; _commandStatusIsGroup = false; }
        UpdatePersistentStatus();
        RebuildRows(); RenderCharts(); RenderAnalysis();
    }

    private void ClearProcessSelection()
    {
        _selectedProcessKey = _selectedGroupKey = null;
        _selectedIdentityVerified = false; _selectedRow = null; _selectedRowWasUnique = false;
    }

    public void SetCommandStatus(string text)
    {
        if (!Dispatcher.CheckAccess()) { Dispatcher.Invoke(() => SetCommandStatus(text)); return; }
        _commandStatus = text; _commandStatusIsGroup = false; UpdatePersistentStatus();
    }

    // 그룹 결과만 별도로 식별해 부분 성공이 앞선 그룹 거절을 대체하게 한다. 지연된
    // 빈 통지는 그 사이 UI/다른 명령이 쓴 안내를 지우지 않으며 다른 세션에는 적용하지 않는다.
    internal void SetGroupCommandStatus(uint session, string text)
    {
        if (!Dispatcher.CheckAccess()) { Dispatcher.Invoke(() => SetGroupCommandStatus(session, text)); return; }
        if (CurrentSnapshot.SessionEpoch != session || (text.Length == 0 && !_commandStatusIsGroup)) return;
        _commandStatus = text; _commandStatusIsGroup = true; UpdatePersistentStatus();
    }

    // 대기는 명령 문자열이 아니라 snapshot의 실제 갱신 상태다. 회복 때는 이 구절만
    // 사라지고, 그동안 생긴 명령 결과·저장 경고·수집 중단 이유는 그대로 표시한다.
    private void UpdatePersistentStatus() => CommandStatusText.Text = CurrentSnapshot.CollectionStatus +
        (string.IsNullOrWhiteSpace(_commandStatus) ? "" : "  ·  " + _commandStatus) +
        (CurrentSnapshot.GroupRefreshPending ? "  ·  그룹 지정을 적용했습니다. 목록 갱신을 기다리는 중입니다." : "");

    public bool SelectProcess(string key, UiMetric? metric = null)
    {
        var row = UiIdentity.FindVerified(CurrentSnapshot, key);
        if (row == null) { SetCommandStatus("대상 인스턴스를 현재 표본에서 확인할 수 없습니다. 다른 프로세스로 이동하지 않았습니다."); return false; }
        var changed = _selectedProcessKey != key;
        _selectedProcessKey = key; _selectedGroupKey = row.GroupKey;
        _selectedIdentityVerified = true;
        _selectedRow = row; _selectedRowWasUnique = true;
        _selectedRoundSession = CurrentSnapshot.SessionEpoch; _selectedRoundSequence = CurrentSnapshot.Sequence;
        SelectedMetric = metric ?? (changed ? DefaultMetric(row) : SelectedMetric);
        _expanded[row.GroupKey] = true;
        var query = SearchBox.Text.Trim();
        bool Matches(string text) => text.Contains(query, StringComparison.CurrentCultureIgnoreCase);
        var groupName = CurrentSnapshot.Groups.FirstOrDefault(g => g.Key == row.GroupKey)?.Name ?? "";
        bool visibleUnderSearch = query.Length == 0 || Matches(row.Name) || Matches(row.Path) || Matches(row.Pid.ToString()) || (_grouped && Matches(groupName));
        if (!visibleUnderSearch)
        {
            _rendering = true; SearchBox.Clear(); _rendering = false;
            SetCommandStatus("대상을 표시하기 위해 검색을 해제했습니다.");
        }
        RebuildRows();
        var item = _visibleRows.FirstOrDefault(r => r.Row?.Key == key);
        if (item != null) ProcessList.ScrollIntoView(item);
        RenderCharts(); RenderAnalysis(); ProcessSelectionChanged?.Invoke(row);
        return true;
    }

    public bool SelectGroup(string key)
    {
        if (!CurrentSnapshot.Groups.Any(g => g.Key == key)) return false;
        ClearProcessSelection(); _selectedGroupKey = key;
        RebuildRows(); RenderCharts(); RenderAnalysis(); ProcessSelectionChanged?.Invoke(null); return true;
    }

    public void SetSidebarVisible(bool visible)
    {
        if (ActualWidth < 1180) _narrowDetail = visible; else _sidebarVisible = visible;
        ApplyLayout();
    }

    public void ExpandAll(bool expand)
    {
        foreach (var group in CurrentSnapshot.Groups) _expanded[group.Key] = expand;
        RebuildRows();
    }

    private static UiMetric DefaultMetric(UiRow row) => row.AlertCauses.FirstOrDefault()?.Metric ?? row.Comparisons.FirstOrDefault(c => c.Candidate)?.Metric ?? UiMetric.Ram;
    private bool IsExpanded(string group) => _expanded.TryGetValue(group, out var expanded) ? expanded : CurrentSnapshot.IsSynthetic;
    private object SortValue(UiRow row) => SortBox.SelectedIndex switch
    { 1 => row.CpuPercent ?? -1, 2 => row.WorkingSetBytes ?? -1, 3 => row.GpuPercent ?? -1, 4 => row.ReadBytesPerSecond ?? -1, 5 => row.WriteBytesPerSecond ?? -1, _ => row.Name.ToUpperInvariant() };
    private object SortValue(UiGroup group) => SortBox.SelectedIndex switch
    { 1 => group.CpuPercent ?? -1, 2 => group.WorkingSetBytes ?? -1, 3 => group.GpuPercent ?? -1, 4 => group.ReadBytesPerSecond ?? -1, 5 => group.WriteBytesPerSecond ?? -1, _ => group.Name.ToUpperInvariant() };
    private sealed record DisplayRow(UiRow Row, string Key);
    private IEnumerable<DisplayRow> Sorted(IEnumerable<DisplayRow> rows) => SortBox.SelectedIndex == 0 ? rows : SortBox.SelectedIndex == 6 ? rows.OrderBy(r => SortValue(r.Row)).ThenBy(r => r.Row.Pid) : rows.OrderByDescending(r => SortValue(r.Row)).ThenBy(r => r.Row.Pid);
    private IEnumerable<UiGroup> Sorted(IEnumerable<UiGroup> groups) => SortBox.SelectedIndex == 0 ? groups : SortBox.SelectedIndex == 6 ? groups.OrderBy(SortValue) : groups.OrderByDescending(SortValue);

    // 현재 복사본 안에서 검색·정렬·접기를 적용한다. 숨긴 구성원도 그룹 합계에는 남고,
    // 재사용하는 ViewListRow만 갱신하므로 여기서는 DB나 그룹 소속 정책을 바꾸지 않는다.
    private void RebuildRows()
    {
        if (ProcessList == null) return;
        _rendering = true;
        try
        {
            var query = SearchBox.Text.Trim();
            bool Matches(string text) => text.Contains(query, StringComparison.CurrentCultureIgnoreCase);
            bool RowMatches(UiRow row) => query.Length == 0 || Matches(row.Name) || Matches(row.Path) || Matches(row.Pid.ToString());
            var counts = CurrentSnapshot.Rows.GroupBy(r => r.Key).ToDictionary(g => g.Key, g => g.Count());
            // 중복 tuple의 슬롯은 이 복사본 안의 목록 위치일 뿐이다. 명령에는 전달하지
            // 않고, 다음 회차의 모호한 선택이나 인스턴스 신원을 이어 붙이는 데도 쓰지 않는다.
            var rows = CurrentSnapshot.Rows.Select((row, index) => new DisplayRow(row, counts[row.Key] == 1
                ? "p:" + row.Key : $"d:{CurrentSnapshot.SessionEpoch}:{CurrentSnapshot.Sequence}:{index}")).ToArray();
            var desired = new List<ViewListRow>();
            ViewListRow Get(DisplayRow? row, UiGroup? group)
            {
                var key = group != null ? "g:" + group.Key : row!.Key;
                if (!_rowCache.TryGetValue(key, out var item)) _rowCache[key] = item = new();
                item.Set(row?.Row, group, group != null && IsExpanded(group.Key), key); return item;
            }
            if (_grouped)
            {
                var members = rows.GroupBy(r => r.Row.GroupKey).ToDictionary(g => g.Key, g => g.ToArray());
                foreach (var group in Sorted(CurrentSnapshot.Groups))
                {
                    var groupRows = members.TryGetValue(group.Key, out var values) ? values : Array.Empty<DisplayRow>();
                    var matching = (query.Length > 0 && Matches(group.Name)) ? groupRows : groupRows.Where(r => RowMatches(r.Row)).ToArray();
                    if (query.Length > 0 && matching.Length == 0) continue;
                    int alertMembers = groupRows.Count(r => r.Row.HasAlert);
                    desired.Add(Get(null, group.AlertCount == alertMembers ? group : group with { AlertCount = alertMembers }));
                    if (IsExpanded(group.Key) || query.Length > 0)
                        desired.AddRange(Sorted(matching).Select(row => Get(row, null)));
                }
            }
            else desired.AddRange(Sorted(rows.Where(r => RowMatches(r.Row))).Select(row => Get(row, null)));
            // 일반 회차에는 기존 행 객체를 이동·재사용해 컨테이너와 스크롤 위치를 유지한다.
            for (var i = 0; i < desired.Count; i++)
            {
                if (i < _visibleRows.Count && ReferenceEquals(_visibleRows[i], desired[i])) continue;
                var index = _visibleRows.IndexOf(desired[i]);
                if (index >= 0) _visibleRows.Move(index, i); else _visibleRows.Insert(i, desired[i]);
            }
            while (_visibleRows.Count > desired.Count) _visibleRows.RemoveAt(_visibleRows.Count - 1);
            var selected = SelectedRow;
            ProcessList.SelectedItem = _selectedProcessKey != null
                ? selected == null ? null : _visibleRows.FirstOrDefault(item => ReferenceEquals(item.Row, selected))
                : _selectedGroupKey == null ? null : _visibleRows.FirstOrDefault(item => item.Group?.Key == _selectedGroupKey);
            var living = rows.Select(r => r.Key).Concat(CurrentSnapshot.Groups.Select(g => "g:" + g.Key)).ToHashSet();
            foreach (var key in _rowCache.Keys.Where(k => !living.Contains(k)).ToArray()) _rowCache.Remove(key);
            foreach (var key in _expanded.Keys.Where(k => !CurrentSnapshot.Groups.Any(g => g.Key == k)).ToArray()) _expanded.Remove(key);
        }
        finally { _rendering = false; }
    }

    private void RenderCharts()
    {
        var group = SelectedRow != null || _selectedProcessKey == null ? SelectedGroup : null;
        ChartTitle.Text = group == null ? "PC 전체 현황" : group.Name + " 그룹 점유율";
        SelectedGroupDot.Visibility = group == null ? Visibility.Collapsed : Visibility.Visible;
        if (group != null) SelectedGroupDot.Fill = DonutChart.BrushOf(group.Color);
        CpuChart.SetChart(CurrentSnapshot.Cpu, group?.Key);
        GpuChart.SetChart(CurrentSnapshot.Gpu, group?.Key);
        RamChart.SetChart(CurrentSnapshot.Ram, group?.Key);
        ChartExplanationText.Text = $"CPU · {CurrentSnapshot.Cpu.Explanation}\nGPU · {CurrentSnapshot.Gpu.Explanation}\nRAM · {CurrentSnapshot.Ram.Explanation}\n목록 RAM은 Working Set / 전체 물리 RAM입니다. 공유 페이지가 중복될 수 있어 원의 그룹 값과 다를 수 있습니다. 모든 %는 검색 전 같은 수집 회차를 기준으로 합니다.";
    }

    private enum AnalysisEvidenceSource { SavedAlert, PersonalComparison, RecentComparison }

    // 선택 결과와 같은 지표의 원본을 함께 보관한다. 화면용 참조이며 이력 조회나 원본 수정은 하지 않는다.
    // Comparison/Recent는 상세 근거 창에도 필요하므로, 카드에 채택되지 않았어도 원본을 유지한다.
    private sealed record AnalysisEvidence(UiMetric Metric, AnalysisEvidenceSource Source,
        UiAlert? Alert, UiComparison? Comparison, UiRecentComparison? Recent);

    // 한 행·한 지표의 표시 우선순위를 결정한다. 이 함수는 컨트롤이나 선택 상태를 바꾸지 않는다.
    // 다른 지표의 준비 여부를 섞지 않아, 같은 행에서 CPU는 장기 비교이고 RAM은 최근 비교일 수 있다.
    private static AnalysisEvidence SelectAnalysisEvidence(UiRow row, UiMetric metric)
    {
        var alert = row.AlertEvidence(metric);
        var comparison = row.Comparisons.FirstOrDefault(c => c.Metric == metric);
        var recent = row.RecentComparisons.FirstOrDefault(r => r.Metric == metric);

        // 알림 당시 상한이 없으면 최신 상한으로 채우지 않는다. 두 값의 관측 구간이 다를 수 있다.
        // 따라서 평균·상한·시각이 일부 빠진 알림도 그 기록 전체를 우선한다.
        if (alert != null) return new(metric, AnalysisEvidenceSource.SavedAlert, alert, comparison, recent);
        if (comparison?.Ready == true) return new(metric, AnalysisEvidenceSource.PersonalComparison, null, comparison, recent);
        // 같은 세션·경로·인스턴스의 최근 이력은 MonitorLoop가 선택 행에만 붙인다.
        // 신원이 유효한 행의 정상 응답 Waiting/Preparing/Ready는 모두 최근 비교로 그린다.
        // LongTerm은 이 분기에서 제외하며, 준비된 장기 비교는 이미 위에서 우선했다.
        if (row.IdentityValid && recent != null && recent.State != RecentComparisonState.LongTerm)
            return new(metric, AnalysisEvidenceSource.RecentComparison, null, comparison, recent);
        // 신원 미확인, LongTerm, 또는 미도착/조회 실패로 recent가 null인 경우에는
        // 기존 개인 비교의 학습/결측 표시를 남긴다. 정상 응답 Waiting과 null은 다르다.
        return new(metric, AnalysisEvidenceSource.PersonalComparison, null, comparison, recent);
    }

    // 현재 선택을 개별·그룹·선택 없음으로 나누고, 개별 카드는 선택한 근거 한 가지로만 갱신한다.
    // 검색·정렬·목록 갱신과 무관한 표시 작업이며, DB 조회와 선택 이벤트를 발생시키지 않는다.
    private void RenderAnalysis()
    {
        var row = SelectedRow; var group = SelectedGroup;
        EmptyAnalysis.Visibility = row == null && (_selectedProcessKey != null || group == null) ? Visibility.Visible : Visibility.Collapsed;
        Analysis.Visibility = EmptyAnalysis.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;
        if (EmptyAnalysis.Visibility == Visibility.Visible)
        {
            EmptyAnalysisText.Text = _selectedProcessKey == null ? "프로세스를 선택하세요" : "선택한 인스턴스를 현재 표본에서 확인할 수 없어요";
            ClearAnalysisDetails(); return;
        }
        bool isGroup = row == null;
        AnalysisMenuButton.Content = isGroup ? "소속·측정 근거" : "원인 지표 ▾";
        AnalysisMenuButton.ToolTip = isGroup ? "그룹 소속 근거와 지표별 측정 범위" : "원인 지표 선택, 측정·판정 근거와 프로그램 정보";
        AutomationProperties.SetName(AnalysisMenuButton, isGroup ? "소속·측정 근거" : "원인 지표와 프로그램 정보 메뉴");
        if (row == null && group != null) { RenderGroupAnalysis(group); return; }
        if (row == null) return;
        var evidence = SelectAnalysisEvidence(row, SelectedMetric);
        RenderProcessAnalysis(row, evidence);
        _evidence = BuildEvidence(row, evidence);
    }

    // 선택 해제·신원 소실 시 숨겨진 컨트롤까지 비운다. 이후 근거 창이나 다른 선택에 이전 숫자와
    // 강조가 남지 않으며, 현재 표본도 이전 인스턴스의 값으로 표시하지 않는다.
    private void ClearAnalysisDetails()
    {
        AnalysisBell.Visibility = Visibility.Collapsed;
        AnalysisTarget.Foreground = DonutChart.BrushOf("#506562");
        AnalysisTarget.Text = AnalysisReason.Text = AnalysisMetric.Text = "";
        AnalysisMetric.Visibility = Visibility.Hidden;
        RecentComparisonNote.Visibility = Visibility.Collapsed;
        RecentComparisonNote.Text = "";
        AverageTitle.Text = UpperTitle.Text = CurrentTitle.Text = "";
        AverageValue.Text = UpperValue.Text = CurrentValue.Text = "";
        AverageTime.Text = UpperTime.Text = CurrentTime.Text = "";
        AverageCard.Tag = CurrentCard.Tag = null;
        SelectedIo.Text = "";
        _evidence = "선택한 프로세스의 현재 신원을 확인할 수 없습니다.";
    }

    // SelectAnalysisEvidence의 결정을 소비한다. 두 비교 렌더러는 서로 배타적이므로, 먼저 쓴 카드를
    // 다른 근거로 덮어쓰지 않는다. 현재값·관측 시각은 항상 선택 행의 이번 표본에서 따로 가져온다.
    private void RenderProcessAnalysis(UiRow row, AnalysisEvidence evidence)
    {
        var metric = evidence.Metric;
        var alert = evidence.Alert;
        AnalysisBell.Visibility = alert != null ? Visibility.Visible : Visibility.Collapsed;
        string metricName = ViewFormat.Metric(metric);
        if (alert == null) AnalysisTarget.Text = $"{row.Name} · {metricName} 분석";
        else if (!alert.HasWindowEvidence) AnalysisTarget.Text = $"{row.Name}의 {metricName} 알림 기록";
        else if (!row.AlertMetrics.Contains(metric)) AnalysisTarget.Text = $"{row.Name}의 {metricName} 알림 기록";
        else if (row.AlertMetrics.Any(m => m != metric)) AnalysisTarget.Text = $"{row.Name}의 {metricName} 알림";
        else AnalysisTarget.Text = $"{row.Name}에 {metricName} 알림 1건";
        AnalysisTarget.Foreground = DonutChart.BrushOf(alert != null ? "#955014" : "#506562");
        AnalysisMetric.Visibility = Visibility.Visible;
        AnalysisMetric.Text = metric switch { UiMetric.Ram => "RAM · Working Set (공유 페이지 포함)", UiMetric.Cpu => "CPU · 전체 논리 CPU 용량 대비", UiMetric.Read => "읽기 · 프로세스 I/O", UiMetric.Write => "쓰기 · 프로세스 I/O", _ => "GPU · 이번 구간의 선택 엔진" };

        if (evidence.Source == AnalysisEvidenceSource.RecentComparison) RenderRecentComparison(row, evidence);
        else RenderWindowComparison(evidence);

        CurrentCard.Tag = "current";
        CurrentTitle.Text = (evidence.Source == AnalysisEvidenceSource.RecentComparison ? "현재 표본 " : "현재 ") + metricName;
        var current = ViewFormat.Current(row, metric);
        bool currentValid = current is double number && double.IsFinite(number) && number >= 0;
        CurrentValue.Text = ViewFormat.Value(metric, current);
        CurrentTime.Text = row.ObservedAt.HasValue
            ? ViewFormat.Time(row.ObservedAt) + (currentValid ? " 측정" : " 관측 · 값 없음")
            : currentValid ? "측정 시각 없음" : "관측 시각 없음 · 값 없음";
        SelectedIo.Text = $"프로세스 I/O  읽기 {ViewFormat.Value(UiMetric.Read, row.ReadBytesPerSecond)}  ·  쓰기 {ViewFormat.Value(UiMetric.Write, row.WriteBytesPerSecond)}";
        if (!string.IsNullOrWhiteSpace(row.Feedback)) SelectedIo.Text += "\n알림 설정 · " + row.Feedback;
    }

    // 알림 또는 개인 비교에서 평균·상한·구간 시각을 같은 원본으로 표시한다.
    // 누락 값은 근거 없음/학습 중으로 남기고, 알림 당시 초과가 확인될 때만 평균 카드를 강조한다.
    private void RenderWindowComparison(AnalysisEvidence evidence)
    {
        var metric = evidence.Metric;
        var alert = evidence.Alert;
        var comparison = evidence.Comparison;
        var average = alert != null ? alert.Average : comparison?.Average;
        var upper = alert != null ? alert.Upper : comparison?.Upper;
        var start = alert != null ? alert.Start : comparison?.Start;
        var end = alert != null ? alert.End : comparison?.End;
        var reason = alert != null ? alert.Reason : comparison?.Reason;
        if (string.IsNullOrWhiteSpace(reason))
            reason = alert != null ? "알림 당시의 상세 사유가 없어요" : comparison?.WindowState ?? "이 지표의 알림 근거가 없어요";
        string? emphasis = null;
        if (alert != null && average is double mean && upper is double limit && mean > limit)
        {
            emphasis = "exceeded";
            var difference = metric is UiMetric.Cpu or UiMetric.Gpu
                ? ViewFormat.Number(mean - limit, 1) + "%p"
                : ViewFormat.Value(metric, mean - limit);
            reason = $"당시 평균이 평소 상한보다 {difference} 높았어요";
        }
        AverageCard.Tag = emphasis;
        AnalysisReason.Text = reason ?? "비교 근거가 없어요";
        RecentComparisonNote.Visibility = Visibility.Collapsed;
        RecentComparisonNote.Text = "";
        AverageTitle.Text = alert != null ? "당시 구간 평균" : "최근 구간 평균";
        UpperTitle.Text = alert != null ? "당시 평소 상한" : "구간 당시 개인 상한";
        AverageValue.Text = average.HasValue ? ViewFormat.Value(metric, average) : "근거 없음";
        // 학습 여부는 설명 문구가 아닌 지표 코드로 읽는다. 알림의 누락 상한은 현재
        // 학습 상태와 관계없이 당시 근거 없음으로 남겨 서로 다른 구간을 섞지 않는다.
        bool learning = alert == null && comparison?.WindowStateCode == MetricState.Learning;
        UpperValue.Text = upper.HasValue ? ViewFormat.Value(metric, upper) : learning ? "학습 중" : "근거 없음";
        AverageTime.Text = ViewFormat.Window(start, end);
        UpperTime.Text = end.HasValue ? ViewFormat.Time(end) + " 기준" : "비교 시각 없음";
    }

    // 최근 비교가 선택된 경우에만 호출한다. 이력의 60분 범위·종료 분 중복 제거·최대 30개 선택은
    // 네이티브 조회의 책임이다. 여기서는 별도 과거 5개와 최신 완료 구간의 유효성을 확인해 표시한다.
    // 과거 4개는 준비 중이다. 최소 개수 변경은 detection/monitor_policy.h와 이 표시 조건을 함께 읽는다.
    private void RenderRecentComparison(UiRow row, AnalysisEvidence evidence)
    {
        var metric = evidence.Metric;
        var recent = evidence.Recent!;
        // 유효한 0은 표시한다. 결측·잘못된 구간은 숫자로 채우지 않고 준비/대기 문구를 남긴다.
        bool complete = recent.Start.HasValue && recent.End > recent.Start &&
            recent.Average is double average && double.IsFinite(average) && average >= 0;
        bool ready = complete && recent.State == RecentComparisonState.Ready && recent.Count >= 5 &&
            recent.Median is double median && double.IsFinite(median) && median >= 0 &&
            recent.HistoryStart.HasValue && recent.HistoryEnd >= recent.HistoryStart;
        AnalysisReason.Text = "최근 기록과 비교";
        RecentComparisonNote.Visibility = Visibility.Visible;
        RecentComparisonNote.Text = ready
            ? "개인 기준 학습 중 · 같은 실행의 직전 60분 기록"
            : $"개인 기준 학습 중 · 최근 비교 준비 {recent.Count}/5개" + (complete ? "" : " · 완료 구간 대기");
        AverageCard.Tag = null;
        AverageTitle.Text = ready ? $"최근 {recent.Count}개 구간 중앙값" : "최근 중앙값";
        AverageValue.Text = ready ? ViewFormat.Value(metric, recent.Median) : "준비 중";
        AverageTime.Text = ready ? ViewFormat.Window(recent.HistoryStart, recent.HistoryEnd) : "과거 유효 기록 5개 필요";
        // 오래된 완료 구간이나 일시정지 중의 표본을 '방금'으로 부르지 않는다. 완료 시각과 현재
        // 관측 시각을 별도로 확인하며, 실제 구간 범위와 길이는 어느 경우에도 그대로 표시한다.
        bool justEnded = complete && !CurrentSnapshot.Paused && recent.End == evidence.Comparison?.End &&
            row.ObservedAt >= recent.End && row.ObservedAt - recent.End <= TimeSpan.FromSeconds(75);
        UpperTitle.Text = justEnded ? "방금 끝난 구간 평균" : "마지막 유효 구간 평균";
        UpperValue.Text = complete ? ViewFormat.Value(metric, recent.Average) : "완료 구간 대기";
        UpperTime.Text = complete ? ViewFormat.Window(recent.Start, recent.End) + $" · {recent.Duration:0.#}초" : "구간 정보 없음";
    }

    private void RenderGroupAnalysis(UiGroup group)
    {
        // 그룹 합계는 개별 프로세스의 비교가 아니다. 이전 선택의 최근 설명과 강조를 이어받지 않는다.
        RecentComparisonNote.Visibility = Visibility.Collapsed;
        RecentComparisonNote.Text = "";
        AnalysisBell.Visibility = Visibility.Collapsed; AnalysisTarget.Foreground = DonutChart.BrushOf("#506562");
        AnalysisTarget.Text = group.Name + " · 그룹 합계";
        var notified = CurrentSnapshot.Rows.Where(r => r.GroupKey == group.Key && r.HasAlert).ToArray();
        AnalysisReason.Text = GroupSummary(group, notified);
        AnalysisMetric.Text = "";
        AnalysisMetric.Visibility = Visibility.Hidden;
        AverageCard.Tag = null;
        CurrentCard.Tag = null;
        AverageTitle.Text = "그룹 CPU"; UpperTitle.Text = "그룹 GPU"; CurrentTitle.Text = "그룹 RAM · Working Set";
        string Covered(string value, int valid) => ViewFormat.WithCoverage(value, valid, group.MemberCount);
        string Observed(int valid) => valid == group.MemberCount && valid > 0 && CurrentSnapshot.ObservedAt.HasValue
            ? ViewFormat.Time(CurrentSnapshot.ObservedAt) + " 측정" : $"{group.MemberCount}개 중 {valid}개 측정";
        AverageValue.Text = Covered(ViewFormat.Percent(group.CpuPercent), group.CpuValid);
        UpperValue.Text = Covered(ViewFormat.Percent(group.GpuPercent), group.GpuValid);
        var ram = group.RamPercent.HasValue ? ViewFormat.Percent(group.RamPercent) : ViewFormat.Value(UiMetric.Ram, group.WorkingSetBytes);
        CurrentValue.Text = Covered(ram, group.RamValid);
        AverageTime.Text = Observed(group.CpuValid);
        UpperTime.Text = Observed(group.GpuValid);
        CurrentTime.Text = Observed(group.RamValid);
        var read = Covered(ViewFormat.Value(UiMetric.Read, group.ReadBytesPerSecond), group.ReadValid);
        var write = Covered(ViewFormat.Value(UiMetric.Write, group.WriteBytesPerSecond), group.WriteValid);
        SelectedIo.Text = $"구성원 I/O 합계   읽기 {read}  ·  쓰기 {write}";
        _evidence = $"{group.Name} · 그룹 합계\n{group.Description}\n{group.Evidence}\n\n" +
            $"목록 RAM · Working Set {Covered(ViewFormat.Value(UiMetric.Ram, group.WorkingSetBytes), group.RamValid)} (공유 페이지 포함)\n" +
            "도넛 RAM은 전용 Working Set을 사용합니다. *는 일부 구성원만 측정한 합계입니다.";
        if (notified.Length > 0)
            _evidence += $"\n\n알림 기록이 있는 구성원 {notified.Length}개. 제목의 지표는 당시 구간, 평균, 상한이 함께 보존된 기록을 기준으로 합니다.";
    }

    private static string GroupSummary(UiGroup group, UiRow[] notified)
    {
        if (notified.Length == 0) return $"구성원 {group.MemberCount}개의 현재 사용량";
        var members = notified.Where(r => r.AlertCauses.Any(a => a.HasWindowEvidence)).ToArray();
        if (members.Length == 0) return "알림 기록의 당시 구간 근거가 일부 없어요";
        var metrics = members.SelectMany(r => r.AlertCauses).Where(a => a.HasWindowEvidence)
            .Select(a => a.Metric).Distinct().OrderBy(m => m).Select(ViewFormat.Metric).ToArray();
        if (members.Length == 1 && metrics.Length == 1) return $"{members[0].Name}에 {metrics[0]} 알림 1건";
        var target = members.Length == 1 ? members[0].Name : $"구성원 {members.Length}개";
        return $"{target}에 알림 · {string.Join("·", metrics)}";
    }

    // 카드 선택에 사용한 같은 지표의 원본을 설명 창용 문자열로 만든다. 채택하지 않은 근거도
    // 출처별로 구분해 보존하며, 여기서 재조회하거나 카드·선택 상태를 변경하지 않는다.
    private string BuildEvidence(UiRow row, AnalysisEvidence selected)
    {
        var alert = selected.Alert;
        var comparison = selected.Comparison;
        var text = new StringBuilder().AppendLine(row.Name).AppendLine($"PID {row.Pid} · 생성 식별 {row.Creation}").AppendLine(row.Path)
            .AppendLine($"현재 관측 {row.ObservedAt:O}").AppendLine(row.Evidence).AppendLine();
        if (SelectedGroup is UiGroup group) text.AppendLine($"그룹 소속 · {group.Name} · {group.Description}").AppendLine();
        if (alert != null) text.AppendLine("알림 당시 보존 근거").AppendLine(alert.Reason).AppendLine($"구간 {alert.Start:O} – {alert.End:O}").AppendLine($"평균 {ViewFormat.Value(alert.Metric, alert.Average)} / 당시 상한 {ViewFormat.Value(alert.Metric, alert.Upper)}").AppendLine(alert.Evidence).AppendLine();
        if (comparison != null) text.AppendLine("최근 완료 구간 비교").AppendLine(comparison.WindowState).AppendLine(comparison.Reason).AppendLine($"구간 {comparison.Start:O} – {comparison.End:O}").AppendLine($"평균 {ViewFormat.Value(comparison.Metric, comparison.Average)} / 당시 상한 {ViewFormat.Value(comparison.Metric, comparison.Upper)}").AppendLine(comparison.Evidence);
        var recent = selected.Recent;
        if (recent != null) text.AppendLine().AppendLine("최근 기록과 비교 · 화면 설명용")
            .AppendLine($"과거 유효 기록 {recent.Count}개 / 상태 {recent.State}")
            .AppendLine($"과거 관측 범위 {recent.HistoryStart:O} – {recent.HistoryEnd:O}")
            .AppendLine($"최근 중앙값 {ViewFormat.Value(recent.Metric, recent.Median)}")
            .AppendLine($"최신 유효 완료 구간 {recent.Start:O} – {recent.End:O}")
            .AppendLine($"완료 구간 평균 {ViewFormat.Value(recent.Metric, recent.Average)} / {recent.Duration:0.0}초")
            .AppendLine(recent.Evidence);
        text.AppendLine().AppendLine("알림 당시와 현재값은 서로 다른 시점입니다. 결측은 0이 아닙니다. 자원 사용만으로 안전·악성·불필요 여부를 판단하지 않습니다.");
        return text.ToString();
    }

    private void OnListSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_rendering || ProcessList.SelectedItem is not ViewListRow item) return;
        if (item.Group is UiGroup group)
        { ClearProcessSelection(); _selectedGroupKey = group.Key; ProcessSelectionChanged?.Invoke(null); }
        else if (item.Row is UiRow row)
        {
            if (!ReferenceEquals(SelectedRow, row)) SelectedMetric = DefaultMetric(row);
            _selectedProcessKey = row.Key; _selectedGroupKey = row.GroupKey;
            _selectedRow = row;
            _selectedIdentityVerified = UiIdentity.IsVerified(CurrentSnapshot, row);
            _selectedRowWasUnique = CurrentSnapshot.Rows.Count(r => r.Key == row.Key) == 1;
            _selectedRoundSession = CurrentSnapshot.SessionEpoch; _selectedRoundSequence = CurrentSnapshot.Sequence;
            ProcessSelectionChanged?.Invoke(row);
        }
        RenderCharts(); RenderAnalysis();
    }
    private void OnExpandRow(object sender, RoutedEventArgs e)
    {
        if (sender is Button { DataContext: ViewListRow { Group: UiGroup group } })
        { _expanded[group.Key] = !IsExpanded(group.Key); RebuildRows(); }
        e.Handled = true;
    }
    private void OnListDoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (ProcessList.SelectedItem is ViewListRow { Group: UiGroup group }) { _expanded[group.Key] = !IsExpanded(group.Key); RebuildRows(); }
    }
    private void OnListKeyDown(object sender, KeyEventArgs e)
    {
        if (ProcessList.SelectedItem is ViewListRow { Group: UiGroup group } && e.Key is Key.Left or Key.Right)
        { _expanded[group.Key] = e.Key == Key.Right; RebuildRows(); e.Handled = true; }
    }
    private void OnListRightClick(object sender, MouseButtonEventArgs e)
    {
        var source = e.OriginalSource as DependencyObject;
        while (source != null && source is not ListBoxItem) source = VisualTreeHelper.GetParent(source);
        if (source is ListBoxItem item) { item.IsSelected = true; OpenContextMenu(ProcessList, MakeGroupMenu()); e.Handled = true; }
    }
    private void OnSearchChanged(object sender, TextChangedEventArgs e) { if (!_rendering && ProcessList != null) RebuildRows(); }
    private void OnSortChanged(object sender, SelectionChangedEventArgs e) { if (!_rendering && ProcessList != null) RebuildRows(); }
    private void OnSortHeader(object sender, RoutedEventArgs e) { if (sender is Button button && int.TryParse(button.Tag?.ToString(), out var index)) SortBox.SelectedIndex = index; }
    private void OnPause(object sender, RoutedEventArgs e) => Request(CurrentSnapshot.Paused ? "resume" : "pause");
    private void Request(string name)
    {
        // 열린 메뉴 뒤 표본이 바뀌어도 표시 슬롯을 실제 프로세스 명령으로 보내지 않는다.
        bool processCommand = name.StartsWith("feedback-", StringComparison.Ordinal) || name is "program-info" or "group-move" or "group-detach" or "group-auto" ||
            (name == "group-target" && _selectedProcessKey != null);
        if (processCommand && !UiIdentity.IsVerified(CurrentSnapshot, SelectedRow))
        { SetCommandStatus("현재 신원이 유일하게 확인된 구성원을 다시 선택하세요."); return; }
        CommandRequested?.Invoke(new UiCommand(name, _selectedProcessKey, _selectedGroupKey, SelectedMetric));
    }
    private void OnMinimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void OnMaximize(object sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void OnClose(object sender, RoutedEventArgs e) => Close();
    private void OnToggleSidebar(object sender, RoutedEventArgs e)
    { if (ActualWidth < 1180) _narrowDetail = !_narrowDetail; else _sidebarVisible = !_sidebarVisible; ApplyLayout(); }
    private void OnToggleToolbar(object sender, RoutedEventArgs e) => Toolbar.Visibility = Toolbar.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;
    private void OnWindowSizeChanged(object sender, SizeChangedEventArgs e) { if (ContentGrid != null) ApplyLayout(); }

    private void ApplyLayout()
    {
        var narrow = ActualWidth < 1180;
        var compact = ActualHeight < 500;
        var showDetail = narrow ? _narrowDetail : _sidebarVisible;
        ConfigureListColumns(!showDetail);
        double minimumContentWidth = showDetail ? 596 : ListColumnsMinimumWidth +
            ListPanel.BorderThickness.Left + ListPanel.BorderThickness.Right + SystemParameters.VerticalScrollBarWidth;
        ContentGrid.Width = Math.Max(minimumContentWidth, ActualWidth - 64);
        PageHeaderRow.Height = new GridLength(compact ? 56 : 74);
        ListTitleRow.Height = new GridLength(compact ? 42 : 55);
        ListHeaderRow.Height = new GridLength(58);
        ListFooter.Visibility = ActualHeight < 440 ? Visibility.Collapsed : Visibility.Visible;
        ToolbarScroll.MaxHeight = compact ? 56 : 130;
        PageTitle.Visibility = ActualWidth < 660 ? Visibility.Collapsed : Visibility.Visible;
        DataModeBadge.Visibility = ActualWidth < 660 ? Visibility.Collapsed : Visibility.Visible;
        ListPanel.Visibility = narrow && showDetail ? Visibility.Collapsed : Visibility.Visible;
        DetailScroll.Visibility = showDetail ? Visibility.Visible : Visibility.Collapsed;
        if (narrow && showDetail) { ListColumn.Width = new GridLength(0); GapColumn.Width = new GridLength(0); DetailColumn.Width = new GridLength(1, GridUnitType.Star); }
        else if (!showDetail) { ListColumn.Width = new GridLength(1, GridUnitType.Star); GapColumn.Width = new GridLength(0); DetailColumn.Width = new GridLength(0); }
        else { ListColumn.Width = new GridLength(Math.Clamp((ActualWidth - 64) * .372, 512, 560)); GapColumn.Width = new GridLength(16); DetailColumn.Width = new GridLength(1, GridUnitType.Star); }
        SidebarLabel.Text = narrow ? showDetail ? "목록 보기" : "분석 보기" : showDetail ? "사이드바 접기" : "사이드바 열기";
        PageTitle.FontSize = ActualWidth < 900 ? 26 : 34;
        MeasuredTime.Visibility = ActualWidth < 900 ? Visibility.Collapsed : Visibility.Visible;
    }

    private static void OpenContextMenu(FrameworkElement target, ContextMenu menu)
    { menu.PlacementTarget = target; menu.Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom; menu.IsOpen = true; }
    private MenuItem Item(string header, string action, bool enabled = true)
    { var item = new MenuItem { Header = header, IsEnabled = enabled }; item.Click += (_, _) => Request(action); return item; }
    private MenuItem LocalItem(string header, Action action)
    { var item = new MenuItem { Header = header }; item.Click += (_, _) => action(); return item; }
    private ContextMenu MakeGroupMenu()
    {
        var menu = new ContextMenu();
        menu.Items.Add(LocalItem("모두 펼치기", () => ExpandAll(true)));
        menu.Items.Add(LocalItem("모두 접기", () => ExpandAll(false)));
        menu.Items.Add(new Separator());
        // 대기 중 그룹 목록은 편집 전의 소속일 수 있다. native의 목록 의존 명령 거절과
        // 같은 조건을 표시하되 현재 tuple을 확인하는 개별 구성원 명령은 계속 허용한다.
        var groupReady = !CurrentSnapshot.GroupRefreshPending && SelectedGroup != null && SelectedGroup.Key != "__other";
        var processReady = UiIdentity.IsVerified(CurrentSnapshot, SelectedRow);
        menu.Items.Add(Item("현재 구성원 확정", "group-confirm", groupReady));
        menu.Items.Add(Item(SelectedRow != null ? "선택 구성원을 이동 대상으로 지정" : "이 그룹을 이동 대상으로 지정", "group-target", SelectedRow != null ? processReady : groupReady));
        menu.Items.Add(Item("선택 구성원을 대상 그룹으로 이동", "group-move", processReady));
        menu.Items.Add(Item("자동 그룹에서 분리 (기타)", "group-detach", processReady));
        menu.Items.Add(Item("자동 추정 소속 복원", "group-auto", processReady));
        return menu;
    }
    private void OnGroupMenu(object sender, RoutedEventArgs e) => OpenContextMenu((FrameworkElement)sender, MakeGroupMenu());
    private void OnNotificationMenu(object sender, RoutedEventArgs e)
    {
        var menu = new ContextMenu();
        menu.Items.Add(Item(CurrentSnapshot.NotificationsEnabled ? "전체 변화 알림 끄기" : "전체 변화 알림 켜기", "toggle-notifications"));
        menu.Items.Add(Item("알림 묶음의 구성원과 당시 근거", "notification-members")); menu.Items.Add(new Separator());
        var enabled = UiIdentity.IsVerified(CurrentSnapshot, SelectedRow) && !string.IsNullOrWhiteSpace(SelectedRow!.Path);
        menu.Items.Add(Item("선택 경로 1시간 알림 억제", "feedback-suppress-hour", enabled));
        menu.Items.Add(Item("선택 경로 24시간 예상 사용", "feedback-expected-day", enabled));
        menu.Items.Add(Item("선택 경로 억제 취소", "feedback-clear", enabled));
        OpenContextMenu((FrameworkElement)sender, menu);
    }
    private void OnMoreMenu(object sender, RoutedEventArgs e)
    {
        var menu = new ContextMenu();
        menu.Items.Add(LocalItem(_grouped ? "개별 프로세스 보기" : "프로그램별 그룹 보기", () => { _grouped = !_grouped; RebuildRows(); }));
        menu.Items.Add(Item("관리자로 다시 실행", "admin-relaunch"));
        menu.Items.Add(Item("이 프로그램 알아보기", "program-info", UiIdentity.IsVerified(CurrentSnapshot, SelectedRow)));
        menu.Items.Add(LocalItem("측정·판정 근거", ShowEvidence));
        if (CurrentSnapshot.IsSynthetic) menu.Items.Add(Item("합성 알림 보내기", "demo-notification"));
        menu.Items.Add(new Separator()); menu.Items.Add(LocalItem("종료", Close));
        OpenContextMenu((FrameworkElement)sender, menu);
    }
    private void OnAnalysisMenu(object sender, RoutedEventArgs e)
    {
        if (SelectedRow == null && SelectedGroup != null) { ShowEvidence(); return; }
        var menu = new ContextMenu();
        foreach (var metric in new[] { UiMetric.Cpu, UiMetric.Ram, UiMetric.Read, UiMetric.Write })
        {
            var captured = metric;
            menu.Items.Add(LocalItem(ViewFormat.Metric(metric) + " 근거", () => { SelectedMetric = captured; RenderAnalysis(); }));
        }
        menu.Items.Add(new Separator());
        menu.Items.Add(Item("이 프로그램 알아보기", "program-info", UiIdentity.IsVerified(CurrentSnapshot, SelectedRow)));
        menu.Items.Add(LocalItem("측정·판정 근거", ShowEvidence));
        OpenContextMenu((FrameworkElement)sender, menu);
    }
    private void ShowEvidence()
    {
        var text = new TextBox { Text = _evidence, IsReadOnly = true, TextWrapping = TextWrapping.Wrap, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(16), FontSize = 14, BorderThickness = new Thickness(0), Background = Brushes.White };
        var dialog = new Window { Owner = this, Title = "측정·판정 근거", Width = 820, Height = 600, MinWidth = 400, MinHeight = 300, WindowStartupLocation = WindowStartupLocation.CenterOwner, Content = text, FontFamily = FontFamily };
        dialog.Show();
    }
}
