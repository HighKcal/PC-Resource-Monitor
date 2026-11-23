using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Controls;
using System.Windows.Automation;

namespace PcMonitor;

/// <summary>전달받은 UiChart로 그리는 도넛이다. 그룹 조각의 합으로 시스템 전체 수치
/// 라벨을 바꾸지 않는다. SetChart의 입력을 OnRender와 선택 이벤트가 함께 사용한다.</summary>
public sealed class DonutChart : Grid
{
    private UiChart _chart = new();
    private string? _selected;
    private string? _hovered;
    private Point? _lastPointer;
    private string _totalText = "전체 —";
    private readonly List<(UiSlice Slice, double Start, double Sweep)> _arcs = new();
    private readonly ToolTip _tip = new() { Placement = System.Windows.Controls.Primitives.PlacementMode.Mouse, StaysOpen = true };
    private static readonly Brush TextBrush = BrushOf("#172D2C");
    private static readonly Brush MutedBrush = BrushOf("#506562");
    private readonly TextBlock _smallCount = new() { FontWeight = FontWeights.Bold };
    private readonly TextBlock _shortcut = new() { FontSize = 10 };
    internal Button SmallGroupsButton { get; }
    internal int SmallGroupCount { get; private set; }
    internal bool ExplorerOpen { get; private set; }
    internal event Action<DonutChart, string, Point>? PreviewRequested;
    internal event Action<DonutChart>? PinRequested;

    public DonutChart()
    {
        MinWidth = 180; Height = 254; VerticalAlignment = VerticalAlignment.Top; Focusable = false;
        var keycap = new Border { Background = Brushes.White, BorderBrush = BrushOf("#C8DCD3"),
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(3), Padding = new Thickness(4,0,4,0),
            Margin = new Thickness(9,0,0,0), Child = _shortcut };
        var caption = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
        caption.Children.Add(_smallCount); caption.Children.Add(keycap);
        SmallGroupsButton = new Button { Name = "SmallGroupsButton", Content = caption,
            HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Top,
            Height = 28, MinHeight = 28, MinWidth = 138, Padding = new Thickness(8,0,8,0),
            Background = BrushOf("#F1F7F4"), BorderBrush = BrushOf("#C8DCD3"), Foreground = BrushOf("#147865"),
            Visibility = Visibility.Collapsed };
        SmallGroupsButton.Click += (_, _) => PinRequested?.Invoke(this);
        Children.Add(SmallGroupsButton);
        TextOptions.SetTextRenderingMode(this, TextRenderingMode.Grayscale);
        MouseMove += OnPointerMove;
        MouseLeftButtonDown += (_, e) => {
            if (e.ChangedButton == MouseButton.Left && HitArc(e.GetPosition(this)) is UiSlice hit && IsSmall(hit))
            { PinRequested?.Invoke(this); e.Handled = true; }
        };
        MouseLeave += (_, _) => { _lastPointer = null; _hovered = null; _tip.IsOpen = false; InvalidateVisual(); };
        Unloaded += (_, _) => _tip.IsOpen = false;
        ToolTip = null;
    }

    public void SetChart(UiChart chart, string? selectedGroup, DateTimeOffset? observedAt = null)
    {
        if (_chart.Total != chart.Total) _totalText = "전체 " + ViewFormat.Percent(chart.Total);
        _chart = chart; _selected = selectedGroup;
        if (_hovered != null && !chart.Slices.Any(s => s.Key == _hovered))
        { _hovered = null; _tip.IsOpen = false; }
        var selected = chart.Slices.FirstOrDefault(s => s.Key == selectedGroup);
        var name = $"{chart.MetricName}, {_totalText}";
        AutomationProperties.SetName(this, selected == null ? name : $"{name}, {selected.Name} {selected.DisplayPercent}");
        BuildArcs();
        SmallGroupCount = _arcs.Count(a => IsSmall(a.Slice));
        UpdateSmallButton();
        if (!ExplorerOpen && _lastPointer is Point point) UpdateHover(point);
        InvalidateVisual();
    }

    private void BuildArcs()
    {
        _arcs.Clear();
        if (!_chart.CanPartition || !_chart.Total.HasValue) return;
        double position = -90;
        foreach (var slice in _chart.Slices.Where(s => s.DrawPercent > 0 && double.IsFinite(s.DrawPercent)))
        {
            var sweep = Math.Min(360 - (position + 90), slice.DrawPercent * 3.6);
            if (sweep <= 0) break;
            _arcs.Add((slice, position, sweep)); position += sweep;
        }
    }

    private Point Center => new(ActualWidth / 2, 103);

    // 탐색 대상은 실제 관측 비율이 0% 초과·4% 미만인 프로그램 조각이다. 그림의
    // DrawPercent로 판정하지 않으며 4% 근처 표시 반올림은 ViewFormat.Percent도 확인한다.
    private static bool IsSmall(UiSlice s) => !s.Free && !s.Unattributed && s.Key != Projection.Other &&
        s.Valid > 0 && s.Percent is > 0 and < 4;

    private void UpdateSmallButton()
    {
        SmallGroupsButton.Visibility = SmallGroupCount > 0 ? Visibility.Visible : Visibility.Collapsed;
        double top = string.IsNullOrEmpty(_chart.Note) ? 248 : 265;
        SmallGroupsButton.Margin = new Thickness(0, top, 0, 0);
        Height = SmallGroupCount == 0 ? 254 : top + 30;
        _smallCount.Text = $"작은 그룹 {SmallGroupCount}개";
        _shortcut.Text = _chart.MetricName switch { "CPU" => "1", "GPU" => "2", _ => "3" };
        AutomationProperties.SetName(SmallGroupsButton, _chart.MetricName + " " + _smallCount.Text);
        AutomationProperties.SetAutomationId(SmallGroupsButton, _chart.MetricName switch {
            "CPU" => "CpuSmallGroups", "GPU" => "GpuSmallGroups", _ => "RamSmallGroups" });
    }

    internal void SetExplorerOpen(bool open)
    {
        ExplorerOpen = open;
        _lastPointer = null; _hovered = null;
        _tip.IsOpen = false;
        InvalidateVisual();
    }

    // 탐색을 열 때 쓸 배열과 호 범위를 고정한다. All은 전체 작은 그룹, Run은 가리킨
    // 조각과 이어진 작은 조각이다. 이후 세션·그룹 생존 확인은 ViewSmallGroups가 맡는다.
    internal SmallGroupRange? CaptureSmallGroups(string? hitKey)
    {
        if (SmallGroupCount == 0) return null;
        var all = _arcs.Where(a => IsSmall(a.Slice)).Select(a => a.Slice).OrderByDescending(s => s.Percent).ToArray();
        if (hitKey == null) return new SmallGroupRange(all, Array.Empty<UiSlice>(), 0, 0);
        int i = _arcs.FindIndex(a => a.Slice.Key == hitKey && IsSmall(a.Slice));
        if (i < 0) return null;
        int first = i, last = i;
        while (first > 0 && IsSmall(_arcs[first - 1].Slice)) --first;
        while (last + 1 < _arcs.Count && IsSmall(_arcs[last + 1].Slice)) ++last;
        var run = _arcs.GetRange(first, last - first + 1).Select(a => a.Slice).OrderByDescending(s => s.Percent).ToArray();
        return new SmallGroupRange(all, run, _arcs[first].Start, _arcs[last].Start + _arcs[last].Sweep);
    }

    internal double? PointerAngle(Point point)
    {
        var offset = point - Center;
        if (offset.Length < 56 || offset.Length > 86) return null;
        return (Math.Atan2(offset.Y, offset.X) * 180 / Math.PI + 450) % 360 - 90;
    }

    private UiSlice? HitArc(Point point) => PointerAngle(point) is double angle
        ? _arcs.FirstOrDefault(a => angle >= a.Start && angle < a.Start + a.Sweep).Slice : null;

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        dc.DrawRectangle(Brushes.Transparent, null, new Rect(RenderSize));
        var center = Center;
        dc.DrawEllipse(null, new Pen(BrushOf("#DEE7E0"), 21), center, 69.5, 69.5);
        if (_chart.Total is double total && double.IsFinite(total))
        {
            if (_chart.CanPartition)
            {
                foreach (var (slice, position, sweep) in _arcs)
                {
                    // 비율은 전달받은 projection 값을 사용한다. 관측되지 않은 나머지를
                    // 숨기려고 확인된 조각의 비율을 다시 늘리지 않는다.
                    var gap = sweep > 2 ? .65 : 0;
                    DrawArc(dc, center, 69.5, position + gap / 2, sweep - gap, BrushOf(slice.Color), 21);
                    if (!slice.Free && slice.Key == _selected)
                        DrawArc(dc, center, 84, position + .3, Math.Max(.1, sweep - .6), TextBrush, 2.6);
                    if (slice.Key == _hovered && slice.Key != _selected)
                        DrawArc(dc, center, 84, position + .3, Math.Max(.1, sweep - .6), BrushOf(slice.Color), 2);
                }
            }
            else
            {
                DrawArc(dc, center, 69.5, -90, Math.Clamp(total, 0, 100) * 3.6, BrushOf("#218D7D"), 21);
            }
        }
        DrawText(dc, _chart.MetricName, 28, FontWeights.Bold, TextBrush, center.X, 73, true);
        DrawText(dc, _totalText, 15, FontWeights.Medium, MutedBrush, center.X, 112, true);
        var selected = _chart.Slices.FirstOrDefault(s => s.Key == _selected);
        if (selected != null)
        {
            var label = selected.Name;
            var value = selected.DisplayPercent;
            var valueText = Text(value, 22, FontWeights.Bold, TextBrush);
            var maxNameWidth = Math.Max(28, ActualWidth - valueText.Width - 25);
            var nameText = Text(label, 14, FontWeights.Medium, MutedBrush);
            nameText.MaxTextWidth = maxNameWidth; nameText.MaxLineCount = 1; nameText.Trimming = TextTrimming.CharacterEllipsis;
            var width = Math.Min(nameText.Width, maxNameWidth) + 7 + valueText.Width;
            dc.DrawText(nameText, new Point(center.X - width / 2, 220));
            dc.DrawText(valueText, new Point(center.X - width / 2 + Math.Min(nameText.Width, maxNameWidth) + 7, 213));
        }
        if (!string.IsNullOrWhiteSpace(_chart.Note))
        {
            var note = Text(_chart.Note, 11, FontWeights.Normal, MutedBrush);
            note.MaxTextWidth = Math.Max(100, ActualWidth - 8); note.MaxLineCount = 1; note.Trimming = TextTrimming.CharacterEllipsis;
            dc.DrawText(note, new Point(4, 244));
        }
    }

    private void OnPointerMove(object sender, MouseEventArgs e)
    {
        _lastPointer = e.GetPosition(this); UpdateHover(_lastPointer.Value);
    }

    private void UpdateHover(Point pointer)
    {
        if (ExplorerOpen) return;
        UiSlice? slice = HitArc(pointer);
        if (slice != null && IsSmall(slice) && PreviewRequested != null)
        {
            _tip.IsOpen = false;
            PreviewRequested(this, slice.Key, pointer);
            return;
        }
        var key = slice?.Key;
        if (_hovered != key) { _hovered = key; InvalidateVisual(); }
        if (slice == null)
        {
            _tip.IsOpen = false;
            _tip.Content = null;
            return;
        }
        _tip.Content = $"{slice.Name.ReplaceLineEndings(" ")} {slice.DisplayPercent}";
        _tip.PlacementTarget = this;
        _tip.IsOpen = true;
    }

    private FormattedText Text(string content, double size, FontWeight weight, Brush brush) =>
        new(content, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface(new FontFamily(size >= 22 ? "pack://application:,,,/Assets/#Inter" : "pack://application:,,,/Assets/#Noto Sans KR"), FontStyles.Normal, weight, FontStretches.Normal),
            size, brush, VisualTreeHelper.GetDpi(this).PixelsPerDip);

    private void DrawText(DrawingContext dc, string content, double size, FontWeight weight, Brush brush, double x, double y, bool centered)
    {
        var text = Text(content, size, weight, brush);
        dc.DrawText(text, new Point(centered ? x - text.Width / 2 : x, y));
    }

    private static void DrawArc(DrawingContext dc, Point center, double radius, double start, double sweep, Brush brush, double thickness)
    {
        if (sweep <= 0) return;
        if (sweep >= 359.999) { dc.DrawEllipse(null, new Pen(brush, thickness), center, radius, radius); return; }
        static Point At(Point c, double r, double angle) => new(c.X + r * Math.Cos(angle * Math.PI / 180), c.Y + r * Math.Sin(angle * Math.PI / 180));
        var geometry = new StreamGeometry();
        using (var context = geometry.Open())
        {
            context.BeginFigure(At(center, radius, start), false, false);
            context.ArcTo(At(center, radius, start + sweep), new Size(radius, radius), 0,
                sweep > 180, SweepDirection.Clockwise, true, false);
        }
        geometry.Freeze(); dc.DrawGeometry(null, new Pen(brush, thickness), geometry);
    }

    internal static Brush BrushOf(string color)
    {
        try { var brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color)); brush.Freeze(); return brush; }
        catch { return Brushes.Gray; }
    }
}
