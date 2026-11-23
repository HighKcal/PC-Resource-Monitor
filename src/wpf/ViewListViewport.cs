using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;

namespace PcMonitor;

// 목록 열 폭과 헤더 위치를 담당하는 MainWindow 부분이다. ApplyLayout이 열 정책을
// 정하고, 레이아웃·스크롤 이후 UpdateListViewport가 실제 항목 뷰포트에 헤더를 맞춘다.
public partial class MainWindow
{
    public static readonly DependencyProperty NameColumnWidthProperty = DependencyProperty.Register(nameof(NameColumnWidth), typeof(GridLength), typeof(MainWindow), new PropertyMetadata(new GridLength(1, GridUnitType.Star)));
    public GridLength NameColumnWidth { get => (GridLength)GetValue(NameColumnWidthProperty); private set => SetValue(NameColumnWidthProperty, value); }
    public static readonly DependencyProperty MetricColumnWidthProperty = DependencyProperty.Register(nameof(MetricColumnWidth), typeof(GridLength), typeof(MainWindow), new PropertyMetadata(new GridLength(72)));
    public GridLength MetricColumnWidth { get => (GridLength)GetValue(MetricColumnWidthProperty); private set => SetValue(MetricColumnWidthProperty, value); }
    public static readonly DependencyProperty MetricColumnMinimumWidthProperty = DependencyProperty.Register(nameof(MetricColumnMinimumWidth), typeof(double), typeof(MainWindow), new PropertyMetadata(0d));
    public double MetricColumnMinimumWidth { get => (double)GetValue(MetricColumnMinimumWidthProperty); private set => SetValue(MetricColumnMinimumWidthProperty, value); }
    public static readonly DependencyProperty ListColumnsMinimumWidthProperty = DependencyProperty.Register(nameof(ListColumnsMinimumWidth), typeof(double), typeof(MainWindow), new PropertyMetadata(490d));
    public double ListColumnsMinimumWidth { get => (double)GetValue(ListColumnsMinimumWidthProperty); private set => SetValue(ListColumnsMinimumWidthProperty, value); }

    private void ConfigureListColumns(bool listOnly)
    {
        // 프로세스 이름과 알림 공간을 남기고 나머지는 자원 열에 균등 배분한다.
        // 좁은 화면은 기존 외부 가로 스크롤로 다섯 값을 보여 준다. 항목을 무한 폭으로
        // 측정하거나 ListBox 가상화를 해제하지 않는다.
        NameColumnWidth = new GridLength(listOnly ? 2.75 : 1, GridUnitType.Star);
        MetricColumnWidth = listOnly ? new GridLength(1, GridUnitType.Star) : new GridLength(72);
        IoColumnWidth = listOnly ? new GridLength(1, GridUnitType.Star) : new GridLength(0);
        MetricColumnMinimumWidth = listOnly ? 104 : 0;
        ListColumnsMinimumWidth = listOnly ? 250 + 5 * 104 : 490;
    }

    private ScrollViewer? _listViewportScroller;
    private ScrollContentPresenter? _listViewportPresenter;
    private ItemsPresenter? _listItemsPresenter;
    private bool _listViewportUpdateQueued;
    private readonly TranslateTransform _listHeaderOffset = new();

    private void OnListViewportLoaded(object sender, RoutedEventArgs e) => RefreshListViewport();
    private void OnListViewportSizeChanged(object sender, SizeChangedEventArgs e) => RefreshListViewport();
    private void OnListViewportVisibilityChanged(object sender, DependencyPropertyChangedEventArgs e) => QueueListViewportUpdate();
    private void OnListViewportScrollChanged(object sender, ScrollChangedEventArgs e) => RefreshListViewport();

    private void RefreshListViewport()
    {
        if (ProcessList.IsLoaded && ProcessList.IsVisible) UpdateListViewport();
        else QueueListViewportUpdate();
    }

    private void QueueListViewportUpdate()
    {
        if (_listViewportUpdateQueued || ProcessList == null) return;
        _listViewportUpdateQueued = true;
        Dispatcher.BeginInvoke(() => {
            _listViewportUpdateQueued = false;
            if (ProcessList.IsLoaded && ProcessList.IsVisible) UpdateListViewport();
        }, DispatcherPriority.Loaded);
    }

    private void UpdateListViewport()
    {
        if (_listViewportScroller is { IsLoaded: false }) {
            _listViewportScroller = null; _listViewportPresenter = null; _listItemsPresenter = null;
        }
        _listViewportScroller ??= FindListVisual<ScrollViewer>(ProcessList);
        if (_listViewportScroller == null) return;
        _listViewportPresenter ??= FindListVisual<ScrollContentPresenter>(_listViewportScroller);
        if (_listViewportPresenter == null || _listViewportPresenter.ActualWidth <= 0) return;
        _listItemsPresenter ??= FindListVisual<ItemsPresenter>(_listViewportPresenter);

        // 고정 스크롤바 여백 대신 실제 항목 viewport를 사용한다. 헤더가 스크롤바의
        // 표시 여부·목록 전용 폭·내부 가로 스크롤 위치를 함께 따라가게 한다.
        var parent = VisualTreeHelper.GetParent(ListHeaderViewport) as UIElement;
        if (parent == null) return;
        var origin = _listViewportPresenter.TranslatePoint(new Point(), parent);
        double viewportWidth = _listViewportPresenter.ActualWidth;
        double contentWidth = Math.Max(viewportWidth, _listItemsPresenter?.ActualWidth ?? viewportWidth);
        ListHeaderViewport.Margin = new Thickness(Math.Max(0, origin.X), 0, 0, 0);
        ListHeaderViewport.Width = viewportWidth;
        ListHeaderGrid.Width = contentWidth;
        _listHeaderOffset.X = -_listViewportScroller.HorizontalOffset;
        ListHeaderGrid.RenderTransform = _listHeaderOffset;
    }

    private static T? FindListVisual<T>(DependencyObject root) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++) {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match) return match;
            if (FindListVisual<T>(child) is T nested) return nested;
        }
        return null;
    }
}
