using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shell;

namespace PcMonitor;

// 작은 도넛 조각의 탐색 상태를 관리한다. InitializeSmallGroupNavigation에서 이벤트를
// 연결하고, 열 때 고정한 범위·세션을 유지하다 선택/닫기에서 정리한다. 수집과는 독립적이다.
public partial class MainWindow
{
    private readonly Canvas _smallGroupLayer = new() { Visibility = Visibility.Collapsed };
    private readonly SmallGroupFlyout _smallGroupFlyout = new();
    private readonly HashSet<string> _retiredSmallGroups = new(StringComparer.Ordinal);
    private DonutChart? _smallSource;
    private SmallGroupRange? _smallRange;
    private long _smallEpoch;
    private DateTimeOffset? _smallObservedAt;
    private Point _smallOrigin;
    private bool _smallPinned;
    private IInputElement? _smallRestoreFocus;

    private void InitializeSmallGroupNavigation()
    {
        Grid.SetRowSpan(_smallGroupLayer, RootGrid.RowDefinitions.Count);
        Panel.SetZIndex(_smallGroupLayer, 20);
        WindowChrome.SetIsHitTestVisibleInChrome(_smallGroupLayer, true);
        _smallGroupLayer.Children.Add(_smallGroupFlyout);
        RootGrid.Children.Add(_smallGroupLayer);
        foreach (var chart in new[] { CpuChart, GpuChart, RamChart })
        {
            chart.PreviewRequested += OpenSmallPreview;
            chart.PinRequested += OpenSmallPinned;
            chart.IsVisibleChanged += (_, _) => { if (chart == _smallSource && !chart.IsVisible) CloseSmallGroups(false); };
            chart.SizeChanged += (_, _) => { if (chart == _smallSource) PositionSmallGroups(); };
        }
        _smallGroupFlyout.PinRequested += () => { if (_smallSource != null) OpenSmallPinned(_smallSource); };
        _smallGroupFlyout.DismissRequested += () => CloseSmallGroups(true);
        _smallGroupFlyout.GroupChosen += ChooseSmallGroup;
        PreviewKeyDown += OnSmallGroupKey;
        PreviewMouseMove += OnSmallGroupMove;
        PreviewMouseDown += OnSmallGroupDown;
        MouseLeave += (_, _) => { if (!_smallPinned) CloseSmallGroups(false); };
        Deactivated += (_, _) => CloseSmallGroups(false);
        Closed += (_, _) => CloseSmallGroups(false);
        RootGrid.SizeChanged += (_, _) => PositionSmallGroups();
        ContentScroll.ScrollChanged += (_, _) => PositionSmallGroups();
    }

    private void OpenSmallPreview(DonutChart chart, string key, Point local)
    {
        if (_smallPinned || chart.CaptureSmallGroups(key) is not { } range) return;
        _smallOrigin = chart.TranslatePoint(local, RootGrid);
        StartSmallGroups(chart, range);
        ShowSmallGroups(false);
    }

    private void OpenSmallPinned(DonutChart chart)
    {
        if (!chart.IsVisible) return;
        if (_smallSource != chart || _smallRange == null)
        {
            if (chart.CaptureSmallGroups(null) is not { } range) return;
            StartSmallGroups(chart, range);
        }
        if (!_smallPinned) _smallRestoreFocus = Keyboard.FocusedElement;
        ShowSmallGroups(true);
        _smallGroupFlyout.FocusList();
    }

    private void StartSmallGroups(DonutChart chart, SmallGroupRange range)
    {
        _smallSource = chart;
        _smallRange = range;
        _smallEpoch = CurrentSnapshot.SessionEpoch;
        _smallObservedAt = CurrentSnapshot.ObservedAt;
        _retiredSmallGroups.Clear();
        SetSmallGroupHover(true);
    }

    private void SetSmallGroupHover(bool open)
    {
        CpuChart.SetExplorerOpen(open);
        GpuChart.SetExplorerOpen(open);
        RamChart.SetExplorerOpen(open);
    }

    private void ShowSmallGroups(bool pinned)
    {
        _smallPinned = pinned;
        _smallGroupLayer.Background = pinned ? Brushes.Transparent : null;
        _smallGroupLayer.Visibility = Visibility.Visible;
        string metric = _smallSource == CpuChart ? "CPU" : _smallSource == GpuChart ? "GPU" : "RAM";
        _smallGroupFlyout.Present(metric, _smallObservedAt, _smallRange!, pinned);
        PositionSmallGroups();
    }

    private void PositionSmallGroups()
    {
        if (_smallSource == null || !IsLoaded) return;
        var chartTop = _smallSource.TranslatePoint(new Point(_smallSource.ActualWidth / 2, 0), RootGrid);
        // 실제 기준점 아래 공간으로 목록을 제한한다. 가상화된 행 컨테이너는 첫 측정
        // 이후에 생성될 수 있으므로 현재 표시 위치를 기준으로 한다.
        const double inset = 8, previewHeight = 180;
        double top = Math.Clamp(chartTop.Y + (_smallSource == CpuChart ? 21 : 34), inset,
            Math.Max(inset, RootGrid.ActualHeight - previewHeight - inset));
        double available = Math.Max(1, RootGrid.ActualHeight - top - inset);
        _smallGroupFlyout.MaxHeight = available;
        _smallGroupFlyout.Measure(new Size(_smallGroupFlyout.Width, available));
        Canvas.SetLeft(_smallGroupFlyout, Math.Clamp(chartTop.X - 257, inset,
            Math.Max(inset, RootGrid.ActualWidth - _smallGroupFlyout.Width - inset)));
        Canvas.SetTop(_smallGroupFlyout, top);
    }

    private void CloseSmallGroups(bool restoreFocus)
    {
        if (_smallSource == null) return;
        var focus = _smallRestoreFocus;
        _smallRestoreFocus = null;
        _smallSource = null;
        _smallRange = null;
        _smallPinned = false;
        _smallGroupLayer.Visibility = Visibility.Collapsed;
        _smallGroupLayer.Background = null;
        _retiredSmallGroups.Clear();
        SetSmallGroupHover(false);
        if (restoreFocus && focus is UIElement { IsVisible: true, IsEnabled: true } target &&
            !_smallGroupFlyout.IsAncestorOf(target))
        {
            // 재사용한 컨테이너가 다른 프로세스를 나타낼 수 있다. 이전 visual 대신
            // 현재 회차에서 선택한 행의 컨테이너로 목록 포커스를 돌린다.
            if (target == ProcessList || ProcessList.IsAncestorOf(target))
            {
                if (ProcessList.SelectedItem != null &&
                    ProcessList.ItemContainerGenerator.ContainerFromItem(ProcessList.SelectedItem) is UIElement current)
                    current.Focus();
                else Focus();
            }
            else target.Focus();
        }
    }

    // 열린 카드의 수치·순서는 바꾸지 않고 생존만 검사한다. 한 번 사라지거나 중복된
    // 키는 같은 카드에서 다시 선택하지 못하게 남겨, 재등장한 그룹으로 이동하지 않는다.
    private void RevalidateSmallGroups(UiSnapshot frame)
    {
        if (_smallRange == null) return;
        if (_smallEpoch != frame.SessionEpoch) { CloseSmallGroups(false); return; }
        var live = new HashSet<string>(StringComparer.Ordinal);
        foreach (var group in frame.Groups)
            if (!live.Add(group.Key)) _retiredSmallGroups.Add(group.Key);
        foreach (var slice in _smallRange.All)
            if (!live.Contains(slice.Key)) _retiredSmallGroups.Add(slice.Key);
    }

    private void ChooseSmallGroup(string key)
    {
        if (!_smallPinned || _smallRange == null || !_smallRange.All.Any(s => s.Key == key)) return;
        bool present = _smallEpoch == CurrentSnapshot.SessionEpoch && !_retiredSmallGroups.Contains(key) &&
            CurrentSnapshot.Groups.Count(g => g.Key == key) == 1;
        CloseSmallGroups(true);
        if (!present)
        {
            SetCommandStatus("선택한 그룹이 현재 표본에서 사라졌습니다. 기존 선택을 유지합니다.");
            return;
        }
        if (!_grouped) { _grouped = true; RebuildRows(); }
        if (SearchBox.Text.Length != 0 && !_visibleRows.Any(r => r.Group?.Key == key))
        {
            SearchBox.Clear();
            SetCommandStatus("선택한 그룹을 표시하기 위해 검색을 해제했습니다.");
        }
        if (SelectGroup(key) && ProcessList.SelectedItem != null) ProcessList.ScrollIntoView(ProcessList.SelectedItem);
    }

    private bool IsInOpenArc(Point point) => _smallSource != null && _smallRange != null &&
        _smallSource.PointerAngle(RootGrid.TranslatePoint(point, _smallSource)) is double angle &&
        angle >= _smallRange.StartAngle && angle < _smallRange.EndAngle;

    private Rect SmallGroupBounds => new(new Point(Canvas.GetLeft(_smallGroupFlyout), Canvas.GetTop(_smallGroupFlyout)),
        _smallGroupFlyout.DesiredSize);

    // 마지막으로 가리킨 호와 카드 사이의 볼록 영역 안에서는 이동 속도와 무관하게
    // 미리보기를 유지한다. 새 표본 대신 열 때 고정한 호와 카드 위치를 사용한다.
    private bool OnPathToSmallGroups(Point point, Rect card)
    {
        card.Inflate(5, 5);
        if (card.Contains(point)) return true;
        static bool InTriangle(Point p, Point a, Point b, Point c)
        {
            double ab = Vector.CrossProduct(b - a, p - a), bc = Vector.CrossProduct(c - b, p - b),
                ca = Vector.CrossProduct(a - c, p - c);
            return (ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0);
        }
        return InTriangle(point, _smallOrigin, card.TopLeft, card.TopRight) ||
               InTriangle(point, _smallOrigin, card.TopRight, card.BottomRight) ||
               InTriangle(point, _smallOrigin, card.BottomRight, card.BottomLeft) ||
               InTriangle(point, _smallOrigin, card.BottomLeft, card.TopLeft);
    }

    private void OnSmallGroupMove(object sender, MouseEventArgs e)
    {
        if (_smallSource == null || _smallPinned) return;
        var point = e.GetPosition(RootGrid);
        if (IsInOpenArc(point)) { _smallOrigin = point; return; }
        if (!OnPathToSmallGroups(point, SmallGroupBounds)) CloseSmallGroups(false);
    }

    private void OnSmallGroupDown(object sender, MouseButtonEventArgs e)
    {
        if (_smallSource == null) return;
        var point = e.GetPosition(RootGrid);
        if (SmallGroupBounds.Contains(point)) return;
        if (!_smallPinned && e.ChangedButton == MouseButton.Left && IsInOpenArc(point))
        { OpenSmallPinned(_smallSource); e.Handled = true; return; }
        bool wasPinned = _smallPinned;
        CloseSmallGroups(wasPinned);
        e.Handled = true;
    }

    private static bool UsesOwnNumberKeys(DependencyObject? element)
    {
        while (element != null)
        {
            // 열린 combo나 메뉴의 항목은 별도 popup visual tree에 속한다.
            if (element is TextBoxBase or PasswordBox or ComboBox or ComboBoxItem or MenuBase or MenuItem or Slider)
                return true;
            element = element is Visual ? VisualTreeHelper.GetParent(element) : LogicalTreeHelper.GetParent(element);
        }
        return false;
    }

    private void OnSmallGroupKey(object sender, KeyEventArgs e)
    {
        if (Keyboard.Modifiers != ModifierKeys.None) return;
        bool editing = SortBox.IsDropDownOpen || UsesOwnNumberKeys(e.OriginalSource as DependencyObject) ||
            UsesOwnNumberKeys(Keyboard.FocusedElement as DependencyObject);
        if (e.Key == Key.Escape && _smallSource != null)
        { CloseSmallGroups(true); e.Handled = !editing; return; }
        if (_smallPinned)
        {
            if (e.Key is Key.Up or Key.Down)
            { _smallGroupFlyout.MoveCursor(e.Key == Key.Up ? -1 : 1); e.Handled = true; }
            else if (e.Key == Key.Enter && _smallGroupFlyout.Choices.IsKeyboardFocusWithin)
            { _smallGroupFlyout.ConfirmCursor(); e.Handled = true; }
            return;
        }
        if (editing) return;
        var chart = e.Key switch { Key.D1 or Key.NumPad1 => CpuChart, Key.D2 or Key.NumPad2 => GpuChart,
            Key.D3 or Key.NumPad3 => RamChart, _ => null };
        if (chart is not { IsVisible: true, SmallGroupCount: > 0 }) return;
        OpenSmallPinned(chart);
        e.Handled = true;
    }
}
