// 작은 그룹 카드의 표시·키보드 커서·선택 이벤트를 맡는다. Present는 열 때 고정한
// 자료만 표시하며, 현재 세션과 그룹 생존 확인 및 실제 선택 이동은 ViewSmallGroups가 맡는다.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace PcMonitor;

internal sealed record SmallGroupRange(UiSlice[] All, UiSlice[] Run, double StartAngle, double EndAngle);
internal sealed record SmallGroupItem(string Key, string Name, string Color, string Share);

public partial class SmallGroupFlyout : UserControl
{
    internal event Action? PinRequested;
    internal event Action? DismissRequested;
    internal event Action<string>? GroupChosen;
    private SmallGroupItem? _pressedRow;

    public SmallGroupFlyout()
    {
        InitializeComponent();
        Choices.PreviewMouseLeftButtonDown += (_, e) => _pressedRow = ItemAt(e.OriginalSource);
        Choices.AddHandler(Mouse.PreviewMouseUpEvent, new MouseButtonEventHandler(OnRowRelease), true);
    }

    private SmallGroupItem? ItemAt(object source) => source is DependencyObject target
        ? (ItemsControl.ContainerFromElement(Choices, target) as ListBoxItem)?.Content as SmallGroupItem : null;

    private void OnRowRelease(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left) return;
        var clicked = ItemAt(e.OriginalSource);
        if (_pressedRow != null && ReferenceEquals(clicked, _pressedRow))
        {
            e.Handled = true;
            GroupChosen?.Invoke(_pressedRow.Key);
        }
        _pressedRow = null;
    }

    internal void Present(string metric, DateTimeOffset? observedAt, SmallGroupRange range, bool pinned)
    {
        _pressedRow = null;
        var visible = pinned ? Visibility.Visible : Visibility.Collapsed;
        var preview = pinned ? Visibility.Collapsed : Visibility.Visible;
        Width = pinned ? 194 : 158;
        Heading.Visibility = Choices.Visibility = KeyBar.Visibility = visible;
        PreviewRows.Visibility = ShowAllButton.Visibility = preview;
        MetricTitle.Text = $"{metric} 작은 그룹 {range.All.Length}개";
        SampleTime.Text = observedAt.HasValue ? ViewFormat.Time(observedAt) + " 기준" : "측정 시각 없음";
        ShowAllButton.Content = $"전체 {range.All.Length}개 보기";
        static IEnumerable<SmallGroupItem> Rows(IEnumerable<UiSlice> slices) =>
            slices.Select(s => new SmallGroupItem(s.Key, s.Name.ReplaceLineEndings(" "), s.Color, s.DisplayPercent));
        PreviewRows.ItemsSource = pinned ? null : Rows(range.Run.Take(5)).ToArray();
        Choices.ItemsSource = pinned ? Rows(range.All).ToArray() : null;
        if (pinned) Choices.SelectedIndex = 0;
    }

    internal void FocusList()
    {
        Choices.Focus();
        Choices.ScrollIntoView(Choices.SelectedItem);
    }

    internal void MoveCursor(int direction)
    {
        int count = Choices.Items.Count;
        if (count == 0) return;
        Choices.SelectedIndex = (Choices.SelectedIndex + direction + count) % count;
        Choices.ScrollIntoView(Choices.SelectedItem);
    }

    internal void ConfirmCursor()
    {
        if (Choices.SelectedItem is SmallGroupItem item) GroupChosen?.Invoke(item.Key);
    }

    private void ShowAllClick(object sender, RoutedEventArgs e) => PinRequested?.Invoke();
    private void CloseClick(object sender, RoutedEventArgs e) => DismissRequested?.Invoke();
}
