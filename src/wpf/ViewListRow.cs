using System;
using System.ComponentModel;
using System.Linq;
using System.Windows;
using System.Windows.Media;

namespace PcMonitor;

// MainWindow.RebuildRows가 키별로 재사용하는 목록 행이다. 개별 값과 그룹 부분합의
// 표기만 계산하며 Set이 복사본 참조를 바꾸고 XAML 바인딩에 전체 속성 갱신을 알린다.
internal sealed class ViewListRow : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;
    public UiRow? Row { get; private set; }
    public UiGroup? Group { get; private set; }
    // 목록 객체 재사용용 키다. UiRow.Key와 달리 같은 tuple의 두 표시 행도 구분한다.
    public string Key { get; private set; } = "";
    public bool Expanded { get; private set; }
    public bool IsGroup => Group != null;
    public string Name => Group?.Name ?? Row!.Name;
    public string Subtitle => Group == null ? "" : Group.AlertCount > 0 ? $"구성원 {Group.AlertCount}개에 알림" :
        Group.Key == "__other" ? $"미지정 {Group.MemberCount}개" : !Expanded ? $"구성원 {Group.MemberCount}개" : "";
    public Visibility SubtitleVisibility => string.IsNullOrEmpty(Subtitle) ? Visibility.Collapsed : Visibility.Visible;
    public FontWeight NameWeight => Group == null ? FontWeights.Medium : FontWeights.Bold;
    public Thickness NameMargin => new(Group == null ? 49 : 53, 0, 3, 0);
    public Visibility GroupVisibility => IsGroup ? Visibility.Visible : Visibility.Collapsed;
    public Visibility MemberVisibility => IsGroup ? Visibility.Collapsed : Visibility.Visible;
    public Visibility AlertVisibility => Row?.HasAlert == true ? Visibility.Visible : Visibility.Collapsed;
    public string AlertLabel
    {
        get {
            var metrics = Row?.AlertMetrics.ToArray();
            return metrics is { Length: > 1 } ? $"알림 지표 {metrics.Length}개" :
                ViewFormat.Metric(metrics is { Length: 1 } ? metrics[0] : UiMetric.Ram) + " 알림";
        }
    }
    public double ChevronAngle => Expanded ? 0 : -90;
    public Brush Background => IsGroup ? DonutChart.BrushOf("#F7F9F7") : Brushes.White;
    public Brush Color => DonutChart.BrushOf(Group?.Color ?? "#7C8892");
    public string Cpu => Group is UiGroup group ? ViewFormat.WithCoverage(ViewFormat.Percent(group.CpuPercent), group.CpuValid, group.MemberCount) : ViewFormat.Percent(Row?.CpuPercent);
    public string Gpu => Group is UiGroup group ? ViewFormat.WithCoverage(ViewFormat.Percent(group.GpuPercent), group.GpuValid, group.MemberCount) : ViewFormat.Percent(Row?.GpuPercent);
    public string Ram => Group is UiGroup group ? ViewFormat.WithCoverage(ViewFormat.Percent(group.PrivateRamPercent), group.PrivateRamValid, group.MemberCount) : ViewFormat.Percent(Row?.PrivateRamPercent);
    public string RamTooltip => Group is UiGroup group
        ? $"전용 RAM {Ram} · {group.PrivateRamValid}/{group.MemberCount}개 측정" +
            (group.PrivateRamValid > 0 && group.PrivateRamValid < group.MemberCount ? " · 부분합" : "")
        : $"전용 RAM {Ram} · 전용 Working Set / 전체 물리 RAM";
    public string Read => Group is UiGroup group ? ViewFormat.WithCoverage(Rate(group.ReadBytesPerSecond), group.ReadValid, group.MemberCount) : Rate(Row?.ReadBytesPerSecond);
    public string Write => Group is UiGroup group ? ViewFormat.WithCoverage(Rate(group.WriteBytesPerSecond), group.WriteValid, group.MemberCount) : Rate(Row?.WriteBytesPerSecond);
    public string Tooltip => Group != null ? $"{Group.Name}\n{RamTooltip}" : $"{Row!.Name} · PID {Row.Pid}\n생성 식별 {Row.Creation}\n{Row.Path}\n{Row.Evidence}";
    private static string Rate(double? value) => value is double v && double.IsFinite(v) && v >= 0 ? ViewFormat.Number(v / 1e6) : "—";
    public void Set(UiRow? row, UiGroup? group, bool expanded = false, string? displayKey = null)
    {
        Key = displayKey ?? (group != null ? "g:" + group.Key : "p:" + row!.Key);
        Row = row; Group = group; Expanded = expanded;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(null));
    }
}
