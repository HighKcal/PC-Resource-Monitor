// 배포 DLL의 Projection → 실제 목록 바인딩·도넛·상세 표시를 같은 합성 입력으로 검사한다.
// 전용 RAM 표시는 개인 기준/최근 비교/알림의 Working Set bytes와 섞지 않는다.
// 실제 수집·사용자 DB·Shell 알림·외부 조회를 실행하지 않는 표시 회귀다.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal static class RamDisplay
{
    private const BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Instance;
    private static readonly DateTimeOffset Now = new(2026, 11, 24, 3, 0, 0, TimeSpan.Zero);
    private static readonly List<object> checks = new(), ledger = new();
    private static readonly Assembly Product = typeof(MainWindow).Assembly;
    private static MainWindow window = null!;
    private static string output = "";
    private static int failures;
    private static ListBox List => (ListBox)window.FindName("ProcessList");
    private static DonutChart Ram => (DonutChart)window.FindName("RamChart");
    private static string Text(string name) => ((TextBlock)window.FindName(name)).Text;
    private static T? Get<T>(object item, string property) => (T?)item.GetType().GetProperty(property, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(item);
    private static object? Field(object item, string name) => item.GetType().GetField(name, Private)!.GetValue(item);
    private static object? Call(object item, string name, params object?[] args) => item.GetType().GetMethod(name, Private)!.Invoke(item, args);
    private static void Check(bool ok, string name, object? detail = null)
    {
        checks.Add(new { name, passed = ok, detail });
        if (!ok) ++failures;
    }
    private static void Scenario(string name, Action action)
    {
        try { action(); }
        catch (Exception e) {
            while (e is TargetInvocationException { InnerException: { } inner }) e = inner;
            Check(false, name + " completed", e.ToString());
        }
    }
    private static void Pump()
    {
        var frame = new DispatcherFrame();
        window.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => frame.Continue = false));
        Dispatcher.PushFrame(frame); window.UpdateLayout();
    }
    private static ProcessRow Row(uint pid, string name, ulong workingSet, ulong? exclusive, double cpu = 0) => new() {
        Key = new(pid, 1), ProgramId = pid, Name = name,
        Valid = ResourceValid.Identity | ResourceValid.Name | ResourceValid.Cpu | ResourceValid.Memory | ResourceValid.Read | ResourceValid.Write,
        MeasuredUtc = (ulong)Now.ToFileTime(), Memory = workingSet, PrivateWorkingSet = exclusive,
        Cpu = cpu, Read = pid * 1e6, Write = pid * 2e6,
        Metric = Enumerable.Range(0, 4).Select(i => new MetricReport {
            State = MetricState.Normal, Current = i == 1 ? 600e6 : 2, Median = i == 1 ? 400e6 : 1,
            Threshold = i == 1 ? 900e6 : 5, Start = (ulong)Now.AddMinutes(-1).ToFileTime(),
            End = (ulong)Now.ToFileTime(), Duration = 60
        }).ToArray()
    };
    private static Snapshot Data(ProcessRow[] rows, params ProcessGroup[] groups) => new() {
        Session = 77, Sequence = 1, Synthetic = true, MeasuredUtc = (ulong)Now.ToFileTime(), Rows = rows, Groups = groups
    };
    private static ProcessGroup Group(string name, ulong id, params int[] members) => new() {
        Key = new(1, new((uint)(id * 10 + 1), 1), id), Label = name, Members = members
    };
    private static UiSnapshot Project(Snapshot data, ulong capacity = 10_000_000_000,
        Dictionary<(ProcessKey,long),UiAlert[]>? alerts = null)
    {
        object sys = Activator.CreateInstance(Product.GetType("PcMonitor.SystemReading", true)!, true)!;
        void Set(string name, object value) => sys.GetType().GetField(name)!.SetValue(sys, value);
        Set("Capacity", capacity); Set("Available", capacity * 4 / 10); Set("Cpu", 20d); Set("Gpu", 10d);
        Set("Utc", data.MeasuredUtc); Set("GpuStart", (ulong)Now.AddSeconds(-2).ToFileTime());
        Set("GpuEnd", (ulong)Now.AddSeconds(-1).ToFileTime());
        Set("GpuProcesses", new Dictionary<uint,double> { [11] = 2, [12] = 1, [21] = 4 });
        object projection = Activator.CreateInstance(Product.GetType("PcMonitor.Projection", true)!, true)!;
        return (UiSnapshot)projection.GetType().GetMethod("Build")!.Invoke(projection,
            [data, sys, true, alerts ?? new()])!;
    }
    private static Snapshot Pair(ulong? first = 300_000_000, ulong? second = 200_000_000) => Data(
        [Row(11, "first", 800_000_000, first, 4), Row(12, "second", 400_000_000, second, 2)], Group("Example", 1, 0, 1));
    private static object[] Items(bool group) => List.Items.Cast<object>().Where(i => (Get<UiGroup>(i, "Group") != null) == group).ToArray();
    private static object GroupItem(string key) => Items(true).Single(i => Get<UiGroup>(i, "Group")!.Key == key);
    private static object ProcessItem(uint pid) => Items(false).Single(i => Get<UiRow>(i, "Row")!.Pid == pid);
    private static string SliceText(UiSlice slice) => Get<string>(slice, "DisplayPercent")!;
    private static bool Coverage(string text, int valid, int total) => text.Contains($"{valid}/{total}") || text.Contains($"{total}개 중 {valid}개 측정");
    private static UiSlice Slice(UiSnapshot frame, string key) => frame.Ram.Slices.Single(s => s.Key == key);
    private static IEnumerable<T> Descendants<T>(DependencyObject parent) where T : DependencyObject
    {
        if (parent is T target) yield return target;
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); ++i)
            foreach (var child in Descendants<T>(VisualTreeHelper.GetChild(parent, i))) yield return child;
    }
    private static void BoundRam(object item, string expected, string name)
    {
        List.ScrollIntoView(item); Pump();
        var visual = List.ItemContainerGenerator.ContainerFromItem(item) as DependencyObject;
        string[] displayed = visual == null ? [] : Descendants<TextBlock>(visual).Select(t => t.Text).ToArray();
        string[] ramCells = visual == null ? [] : Descendants<TextBlock>(visual)
            .Where(t => BindingOperations.GetBindingExpression(t, TextBlock.TextProperty)?.ParentBinding.Path.Path == "Ram")
            .Select(t => t.Text).ToArray();
        Check(Get<string>(item, "Ram") == expected && ramCells.Length == 1 && ramCells[0] == expected, name,
            new { expected, model = Get<string>(item, "Ram"), ramCells, displayed });
    }
    private static string Hover(UiSlice slice)
    {
        // 실제 HitArc→UpdateHover의 말풍선 문자열을 읽는다. 4% 미만 조각은 별도 탐색 카드다.
        var chart = (UiChart)Field(Ram, "_chart")!;
        double start = -90;
        foreach (var s in chart.Slices) {
            if (s.Key == slice.Key) break;
            start += s.DrawPercent * 3.6;
        }
        double angle = (start + slice.DrawPercent * 1.8) * Math.PI / 180;
        Call(Ram, "UpdateHover", new Point(Ram.ActualWidth / 2 + Math.Cos(angle) * 69.5, 103 + Math.Sin(angle) * 69.5));
        string text = ((ToolTip)Field(Ram, "_tip")!).Content?.ToString() ?? "";
        ((ToolTip)Field(Ram, "_tip")!).IsOpen = false;
        return text;
    }
    private static void Observe(string name)
    {
        Pump();
        ledger.Add(new { stage = name, rows = List.Items.Cast<object>().Select(i => new {
            name = Get<string>(i, "Name"), ram = Get<string>(i, "Ram"), cpu = Get<string>(i, "Cpu"), gpu = Get<string>(i, "Gpu")
        }).ToArray(), chart = AutomationProperties.GetName(Ram), current = Text("CurrentValue"), currentTitle = Text("CurrentTitle"),
            average = Text("AverageValue"), upper = Text("UpperValue"), time = Text("CurrentTime"), averageTime = Text("AverageTime") });
        var bitmap = new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth), (int)Math.Ceiling(window.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(window); var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = File.Create(Path.Combine(output, name + ".png")); encoder.Save(stream);
    }
    private static void Primary(bool before)
    {
        var frame = Project(Pair()); var group = frame.Groups.Single();
        window.RenderSnapshot(frame); window.ExpandAll(true); window.SelectGroup(group.Key); Pump();
        var item = GroupItem(group.Key); string tooltip = Hover(Slice(frame, group.Key));
        ledger.Add(new { stage = "same-input-mismatch", list = Get<string>(item, "Ram"), donut = SliceText(Slice(frame, group.Key)),
            selectedLabel = AutomationProperties.GetName(Ram), tooltip, detail = Text("CurrentValue"), group.RamPercent,
            group.WorkingSetBytes, system = frame.Ram.Total });
        BoundRam(item, before ? "12%" : "5%", "same input real bound group RAM " + (before ? "12% before" : "5% after"));
        Check(SliceText(Slice(frame, group.Key)) == "5%" && AutomationProperties.GetName(Ram).Contains("Example 5%") && tooltip.Contains("Example 5%"),
            "same frame donut slice selected label and real hover are 5%", new { tooltip, selected = AutomationProperties.GetName(Ram) });
        Check(Text("CurrentValue") == "12%" && Text("CurrentTitle").Contains("Working Set") && group.WorkingSetBytes == 1.2e9,
            "group detail remains inclusive Working Set 12%", new { title = Text("CurrentTitle"), value = Text("CurrentValue"), group.WorkingSetBytes });
        Check(Get<string>(item, "Cpu") == "6%" && Get<string>(item, "Gpu") == "3%" && frame.Cpu.Slices.Single(s => s.Key == group.Key).Percent == 6 &&
            frame.Gpu.Slices.Single(s => s.Key == group.Key).Percent == 3 && frame.Cpu.Total == 20 && frame.Gpu.Total == 10,
            "CPU GPU and system totals retain original scope");
        Check(frame.Ram.Total == 60 && Text("RamTotal").Contains("60%"), "system RAM total is independent 60%", Text("RamTotal"));
        if (!before) {
            BoundRam(ProcessItem(11), "3%", "first process bound row uses private RAM 3%");
            BoundRam(ProcessItem(12), "2%", "second process bound row uses private RAM 2%");
            Check(Coverage(tooltip, 2, 2), "RAM tooltip reports complete measurement coverage", tooltip);
            Check(AutomationProperties.GetName(Ram).Contains("PC 전체"), "RAM donut names its total as PC total", AutomationProperties.GetName(Ram));
            var sort = (ComboBox)window.FindName("SortBox");
            Check(((ComboBoxItem)sort.Items[2]).Content.ToString()!.Contains("전용 RAM"), "RAM sort caption names private RAM");
            Check(Descendants<Button>(window).Any(b => b.Tag?.ToString() == "2" && Descendants<TextBlock>(b).Any(t => t.Text.Contains("전용 RAM")) &&
                Descendants<TextBlock>(b).Any(t => t.Text == "PC 전체") && Descendants<TextBlock>(b).Any(t => t.Text == "60%")),
                "private RAM column labels header total as PC total 60%");
        }
        Observe(before ? "before-12-vs-5" : "after-5-vs-5");
    }
    private static void MissingAndOther()
    {
        var nativeSums = Pair();
        nativeSums = nativeSums with { Groups = [nativeSums.Groups.Single() with { Metric = [
            new() { Value = 6, Valid = 2, Total = 2 }, new() { Bytes = 1_200_000_000, Valid = 2, Total = 2 },
            new() { Value = 23e6, Valid = 2, Total = 2 }, new() { Value = 46e6, Valid = 2, Total = 2 }
        ] }] };
        var nativeFrame = Project(nativeSums); window.RenderSnapshot(nativeFrame); window.ExpandAll(true); window.SelectGroup(nativeFrame.Groups.Single().Key); Pump();
        BoundRam(GroupItem(nativeFrame.Groups.Single().Key), "5%", "native inclusive group aggregate is not reused as private display sum");
        Check(Text("CurrentValue") == "12%" && nativeFrame.Groups.Single().WorkingSetBytes == 1.2e9,
            "native inclusive group aggregate still supplies Working Set detail");
        foreach (var pair in new[] { ((ulong?)300_000_000, (ulong?)null, "3%*", 1), ((ulong?)null, (ulong?)null, "—", 0), ((ulong?)0, (ulong?)0, "0%", 2) }) {
            var frame = Project(Pair(pair.Item1, pair.Item2)); var group = frame.Groups.Single(); var slice = Slice(frame, group.Key);
            window.RenderSnapshot(frame); window.ExpandAll(true); window.SelectGroup(group.Key); Pump();
            BoundRam(GroupItem(group.Key), pair.Item3, "private coverage group " + pair.Item3);
            Check(SliceText(slice) == pair.Item3 && slice.Valid == pair.Item4 && slice.Total == 2,
                "list and chart share private sum and valid count " + pair.Item3, slice);
            BoundRam(ProcessItem(11), pair.Item1 == null ? "—" : pair.Item1 == 0 ? "0%" : "3%", "first private row missing/zero/value " + pair.Item3);
            BoundRam(ProcessItem(12), pair.Item2 == null ? "—" : "0%", "second private row missing/zero " + pair.Item3);
            Check(Text("CurrentValue") == "12%" && group.RamValid == 2, "missing private data never changes inclusive group detail " + pair.Item3);
            Check(AutomationProperties.GetName(Ram).Contains("Example " + pair.Item3), "selected group label preserves coverage " + pair.Item3);
            if (pair.Item4 == 1) {
                string tip = Get<string>(GroupItem(group.Key), "Tooltip")!;
                Check(Coverage(tip, 1, 2) && tip.Contains("부분합"), "partial list tooltip explains measured members and partial sum", tip);
            }
        }
        // 큰 부분합 조각은 실제 도넛 말풍선을 이용한다.
        var partial = Project(Pair(500_000_000, null)); var pg = partial.Groups.Single();
        window.RenderSnapshot(partial); window.SelectGroup(pg.Key); Pump(); string hover = Hover(Slice(partial, pg.Key));
        Check(hover.Contains("5%*") && Coverage(hover, 1, 2) && hover.Contains("부분합"), "partial RAM hover states same value and coverage", hover);
        var independent = Pair() with { Rows = Pair().Rows.Select(r => r with { Valid = r.Valid & ~ResourceValid.Memory }).ToArray() };
        var separate = Project(independent); window.RenderSnapshot(separate); window.ExpandAll(true); window.SelectGroup(separate.Groups.Single().Key); Pump();
        BoundRam(GroupItem(separate.Groups.Single().Key), "5%", "valid private RAM does not require inclusive Working Set validity");
        Check(Text("CurrentValue") == "—" && separate.Groups.Single().RamValid == 0 && SliceText(Slice(separate, separate.Groups.Single().Key)) == "5%",
            "inclusive missing coverage and private valid coverage remain separate", new { current = Text("CurrentValue"), inclusiveValid = separate.Groups.Single().RamValid });
        var other = Project(Pair() with { Groups = [] }); var og = other.Groups.Single();
        window.RenderSnapshot(other); window.ExpandAll(true); window.SelectGroup(og.Key); Pump();
        BoundRam(GroupItem(og.Key), "5%", "other bucket bound list sums private RAM");
        Check(og.Key == "__other" && SliceText(Slice(other, og.Key)) == "5%", "other bucket RAM chart matches list");
        var noCapacity = Project(Pair(), 0); window.RenderSnapshot(noCapacity); window.ExpandAll(true); Pump();
        BoundRam(GroupItem(noCapacity.Groups.Single().Key), "—", "missing physical capacity remains unavailable, not zero");
        Check(noCapacity.Ram.Total == null && Slice(noCapacity, noCapacity.Groups.Single().Key).Percent == null, "capacity missing is preserved by chart");
        Observe("missing-capacity");
    }
    private static void SortAndSearch()
    {
        var frame = Project(Data([Row(11, "large shared", 800_000_000, 100_000_000, 4), Row(12, "large private", 400_000_000, 400_000_000, 2),
            Row(21, "beta member", 700_000_000, 600_000_000, 3)], Group("Alpha", 1, 0, 1), Group("Beta", 2, 2)));
        window.RenderSnapshot(frame); window.ExpandAll(true); var sort = (ComboBox)window.FindName("SortBox"); sort.SelectedIndex = 2; Pump();
        Check(Items(true).Select(i => Get<string>(i, "Name")).SequenceEqual(["Beta", "Alpha"]), "private RAM group order reverses inclusive Working Set order");
        Check(Items(false).Select(i => Get<string>(i, "Name")).SequenceEqual(["beta member", "large private", "large shared"]), "private RAM process ordering follows private bytes");
        window.SelectGroup(frame.Groups.Single(g => g.Name == "Alpha").Key);
        ((TextBox)window.FindName("SearchBox")).Text = "large shared"; Pump();
        var alpha = frame.Groups.Single(g => g.Name == "Alpha");
        BoundRam(GroupItem(alpha.Key), "5%", "search keeps full group private sum");
        Check(SliceText(Slice(frame, alpha.Key)) == "5%" && AutomationProperties.GetName(Ram).Contains("Alpha 5%"), "search does not narrow chart or selected group scope");
        ((TextBox)window.FindName("SearchBox")).Clear(); sort.SelectedIndex = 1; Pump();
        Check(Items(true).Select(i => Get<string>(i, "Name")).SequenceEqual(["Alpha", "Beta"]), "CPU sort is preserved");
        sort.SelectedIndex = 3; Pump();
        Check(Items(true).Select(i => Get<string>(i, "Name")).SequenceEqual(["Beta", "Alpha"]), "GPU sort is preserved");
        Observe("private-order-and-search");
        var zero = Project(Data([Row(31, "A missing", 900_000_000, null), Row(32, "Z zero", 0, 0)], Group("Zero and missing", 3, 0, 1)));
        window.RenderSnapshot(zero); window.ExpandAll(true); sort.SelectedIndex = 2; Pump();
        Check(Items(false).Select(i => Get<string>(i, "Name")).SequenceEqual(["Z zero", "A missing"]), "valid private zero sorts ahead of unavailable private RAM");
        // 분모가 없으면 표시 비율도 정렬 비율도 없다. 숨겨진 bytes로 순위를 만들지 않는다.
        var unknownRatio = Project(Data([Row(32, "higher hidden bytes", 500_000_000, 300_000_000),
            Row(31, "lower hidden bytes", 500_000_000, 100_000_000), Row(21, "largest hidden bytes", 500_000_000, 600_000_000)],
            Group("Alpha", 1, 0, 1), Group("Beta", 2, 2)), 0);
        window.RenderSnapshot(unknownRatio); window.ExpandAll(true); Pump();
        foreach (var group in unknownRatio.Groups) BoundRam(GroupItem(group.Key), "—", "unknown denominator group displays no private ratio " + group.Name);
        Check(Items(true).Select(i => Get<string>(i, "Name")).SequenceEqual(["Alpha", "Beta"]),
            "unavailable private ratios retain stable group tie order rather than hidden byte ranking");
        Check(Items(false).Select(i => Get<UiRow>(i, "Row")!.Pid).SequenceEqual([31u, 32u, 21u]),
            "unavailable private ratios use existing PID tie order within each group");
        sort.SelectedIndex = 0;
    }
    private static void Evidence()
    {
        var data = Pair(); var frame = Project(data); window.RenderSnapshot(frame); window.ExpandAll(true);
        Check(window.SelectProcess(frame.Rows[0].Key, UiMetric.Ram), "verified instance can open RAM detail");
        Check(Text("CurrentValue") == "0.80 GB" && Text("AverageValue") == "0.60 GB" && Text("UpperValue") == "0.90 GB" && Text("AnalysisMetric").Contains("공유 페이지 포함"),
            "personal comparison and current RAM remain inclusive Working Set bytes", new { current = Text("CurrentValue"), average = Text("AverageValue"), upper = Text("UpperValue") });
        var alert = new UiAlert { Metric = UiMetric.Ram, Average = 1.1e9, Upper = 700e6, Start = Now.AddMinutes(-4), End = Now.AddMinutes(-3) };
        frame = Project(data, alerts: new() { [(data.Rows[0].Key, data.Rows[0].ProgramId)] = [alert] });
        window.RenderSnapshot(frame); window.SelectProcess(frame.Rows[0].Key, UiMetric.Ram);
        Check(Text("AverageTitle") == "당시 구간 평균" && Text("AverageValue") == "1.10 GB" && Text("UpperValue") == "0.70 GB" && Text("CurrentValue") == "0.80 GB" && Text("AverageTime").Contains(Now.AddMinutes(-4).ToLocalTime().ToString("HH:mm:ss")) && Text("CurrentTime").Contains(Now.ToLocalTime().ToString("HH:mm:ss")),
            "accepted alert retains inclusive historical values and separate current time", new { average = Text("AverageValue"), upper = Text("UpperValue"), current = Text("CurrentValue"), when = Text("AverageTime"), now = Text("CurrentTime") });
        var recent = new UiRecentComparison { Metric = UiMetric.Ram, State = RecentComparisonState.Ready, Count = 5,
            Median = 400e6, Average = 650e6, Start = Now.AddMinutes(-1), End = Now, HistoryStart = Now.AddMinutes(-10), HistoryEnd = Now.AddMinutes(-2), Duration = 60 };
        var row = frame.Rows[0] with { Alert = null, Alerts = [], Comparisons = [new() { Metric = UiMetric.Ram, StateCode = MetricState.Learning, End = Now }], RecentComparisons = [recent] };
        frame = frame with { Rows = [row, frame.Rows[1]] }; window.RenderSnapshot(frame); window.SelectProcess(row.Key, UiMetric.Ram);
        Check(Text("AverageValue") == "0.40 GB" && Text("UpperValue") == "0.65 GB" && Text("CurrentValue") == "0.80 GB" && Text("AverageTitle").Contains("최근 5개"),
            "first-day recent comparison retains inclusive bytes independently of 3% private list", new { average = Text("AverageValue"), latest = Text("UpperValue"), current = Text("CurrentValue") });
        BoundRam(ProcessItem(11), "3%", "inclusive alert and recent details do not replace private list RAM");
        Observe("inclusive-comparison-alert-recent");
    }
    private static void FrozenSmallGroups()
    {
        var frame = Project(Pair(100_000_000, 100_000_000)); window.RenderSnapshot(frame); Pump();
        Call(window, "OpenSmallPinned", Ram); Pump();
        object flyout = Field(window, "_smallGroupFlyout")!;
        var choices = (ListBox)((FrameworkElement)flyout).FindName("Choices");
        string first = Get<string>(choices.Items[0], "Share")!;
        string stamp = ((TextBlock)((FrameworkElement)flyout).FindName("SampleTime")).Text;
        Check(first == "2%" && (bool)Field(window, "_smallPinned")!, "small RAM list pins initial private observation", new { first, stamp });
        var nextData = Pair(200_000_000, 200_000_000) with { Sequence = 2, MeasuredUtc = (ulong)Now.AddSeconds(10).ToFileTime() };
        var next = Project(nextData); window.RenderSnapshot(next); window.ExpandAll(true); Pump();
        Check(Get<string>(choices.Items[0], "Share") == "2%" && ((TextBlock)((FrameworkElement)flyout).FindName("SampleTime")).Text == stamp,
            "pinned small group value and timestamp stay frozen after next observation");
        BoundRam(GroupItem(next.Groups.Single().Key), "4%", "live list advances while pinned observation stays 2%");
        Check(SliceText(Slice(next, next.Groups.Single().Key)) == "4%", "live donut advances to current private RAM 4%");
        Call(flyout, "ConfirmCursor"); Pump();
        Check(window.SelectedGroup?.Key == next.Groups.Single().Key && AutomationProperties.GetName(Ram).Contains("Example 4%"), "frozen card selection resolves current group with current chart value");
        Observe("frozen-small-group");
        Call(window, "CloseSmallGroups", false);
    }
    [STAThread] private static int Main(string[] args)
    {
        if (args.Length < 3) return 2;
        output = Path.GetFullPath(args[0]); if (Directory.Exists(output)) return 2; Directory.CreateDirectory(output);
        CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("ko-KR"); CultureInfo.CurrentUICulture = CultureInfo.GetCultureInfo("ko-KR");
        string managedPath = Product.Location, managedHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(managedPath)));
        string nativePath = Path.Combine(AppContext.BaseDirectory, "MonitorBridge.dll"), nativeHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(nativePath)));
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        IntPtr native = IntPtr.Zero;
        bool before = args.Contains("--observe-before");
        try {
            Check(managedHash.Equals(args[1], StringComparison.OrdinalIgnoreCase), "loaded managed DLL matches distribution SHA", new { managedPath, managedHash });
            native = NativeLibrary.Load(nativePath);
            Check(nativeHash.Equals(args[2], StringComparison.OrdinalIgnoreCase), "loaded native DLL matches distribution SHA", new { nativePath, nativeHash });
            window = new MainWindow { Width = 1440, Height = 900, ShowActivated = false, ShowInTaskbar = false, Left = -32000, Top = -32000 };
            window.Show(); Pump(); Scenario("same-input display", () => Primary(before));
            if (!before) {
                Scenario("missing values and other", MissingAndOther); Scenario("private sort and search", SortAndSearch);
                Scenario("inclusive comparisons and alerts", Evidence); Scenario("frozen small groups", FrozenSmallGroups);
            }
        } catch (Exception e) { Check(false, "runner setup", e.ToString()); }
        finally { if (window != null) window.Close(); if (native != IntPtr.Zero) NativeLibrary.Free(native); }
        File.WriteAllText(Path.Combine(output, "results.json"), JsonSerializer.Serialize(new {
            checks = checks.Count, failures, managedSha256 = managedHash, nativeSha256 = nativeHash, before, results = checks, ledger,
            syntheticProcessAndSystemReadings = true, productionProjectionAndWpf = true, actualShellOrExternalLookup = false, realSystemCollection = false
        }, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"RAM display checks={checks.Count} failures={failures}; results: {output}");
        return failures == 0 ? 0 : 1;
    }
}
