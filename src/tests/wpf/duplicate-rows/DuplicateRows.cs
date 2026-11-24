// 배포 DLL의 실제 WPF 목록·카드와 worker→native 명령을 검사한다. 중복 행은 읽기 전용
// UiSnapshot 사본에만 주입한다. C의 실제 단일 인스턴스와 같은 tuple도 주입하여 잘못
// 전달된 명령이 native에서 성공할 수 있는 반례를 만들며, 실사용 DB는 열지 않는다.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal static class DuplicateRows
{
    private const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    private static readonly List<object> checks = new(), ledger = new();
    private static readonly List<UiCommand> requests = new();
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    private static readonly DateTimeOffset Now = new(2026, 11, 23, 3, 0, 0, TimeSpan.Zero);
    private static MainWindow window = null!;
    private static object controller = null!, loop = null!;
    private static Type controllerType = null!, loopType = null!;
    private static string output = "";
    private static int failures;
    private static bool started;
    private static UiSnapshot? published;
    private static ListBox List => (ListBox)window.FindName("ProcessList");

    [DllImport("MonitorBridge.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr bridge_current(IntPtr bridge);

    private static string Text(string name) => ((TextBlock)window.FindName(name)).Text;
    private static T? Property<T>(object item, string property) => (T?)item.GetType().GetProperty(property)!.GetValue(item);
    private static object[] Members() => List.Items.Cast<object>().Where(i => Property<UiRow>(i, "Row") != null).ToArray();
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
            Check(false, name + " completed without a list or dispatcher exception", e.ToString());
            ledger.Add(new { stage = name, error = e.ToString(), visible = ObserveList() });
        }
    }
    private static object ObserveList() => List.Items.Cast<object>().Select(i => new {
        name = Property<string>(i, "Name"), cpu = Property<string>(i, "Cpu"), ram = Property<string>(i, "Ram"),
        read = Property<string>(i, "Read"), write = Property<string>(i, "Write"), row = Property<UiRow>(i, "Row")
    }).ToArray();
    private static void Until(Func<bool> ready, string name, int timeout = 10000)
    {
        var time = Stopwatch.StartNew();
        while (!ready()) {
            if (time.ElapsedMilliseconds > timeout) throw new TimeoutException(name);
            var frame = new DispatcherFrame();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(10) };
            timer.Tick += (_, _) => { timer.Stop(); frame.Continue = false; };
            timer.Start(); Dispatcher.PushFrame(frame);
        }
    }
    private static void Pump()
    {
        bool done = false;
        window.Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => done = true));
        Until(() => done, "dispatcher idle");
    }
    private static UiRow Row(string name, double cpu, double memory, bool valid = false, uint pid = 123, ulong creation = 456)
        => new() {
            Name = name, Pid = pid, Creation = creation, IdentityValid = valid, Path = "SYNTHETIC " + name,
            ProgramId = "7", GroupKey = "fixture", CpuPercent = cpu, WorkingSetBytes = memory,
            RamPercent = memory / 1e8, PrivateWorkingSetBytes = memory, PrivateRamPercent = memory / 1e8,
            ReadBytesPerSecond = cpu / 11 * 1e6,
            WriteBytesPerSecond = (cpu / 11 + 2) * 1e6, ObservedAt = Now,
            Comparisons = [new() { Metric = UiMetric.Cpu, StateCode = MetricState.Normal, Average = cpu / 2,
                Upper = cpu * 2, Start = Now.AddMinutes(-1), End = Now }]
        };
    private static UiSnapshot Frame(UiRow[] rows, long sequence = 1, long session = 10) => new() {
        SessionEpoch = session, Sequence = sequence, ObservedAt = Now, IsSynthetic = false, Status = "합성 중복 행",
        Rows = rows, Groups = [new() { Key = "fixture", Name = "중복 반례 그룹", MemberCount = rows.Length,
            CpuPercent = rows.Sum(r => r.CpuPercent ?? 0), CpuValid = rows.Length }]
    };
    private static void Select(UiRow row)
    {
        var item = Members().Single(i => ReferenceEquals(Property<UiRow>(i, "Row"), row));
        List.SelectedItem = item;
        window.UpdateLayout();
        Check(ReferenceEquals(window.SelectedRow, row), "list selection resolves the exact displayed " + row.Name,
            new { selected = window.SelectedRow?.Name, current = Text("CurrentValue"), target = Text("AnalysisTarget") });
    }
    private static void SelectMetric(UiMetric metric)
    {
        var property = typeof(MainWindow).GetProperty(nameof(MainWindow.SelectedMetric))!;
        property.GetSetMethod(true)!.Invoke(window, [metric]);
        typeof(MainWindow).GetMethod("RenderAnalysis", Private)!.Invoke(window, null);
    }
    private static void SafeSelection(UiRow expected, string name)
        => Check(window.SelectedRow == null || ReferenceEquals(window.SelectedRow, expected), name,
            new { selected = window.SelectedRow?.Name, current = Text("CurrentValue"), target = Text("AnalysisTarget") });
    private static void Capture(string name)
    {
        window.UpdateLayout();
        ledger.Add(new { stage = name, visible = ObserveList(), selected = window.SelectedRow?.Name,
            target = Text("AnalysisTarget"), current = Text("CurrentValue"), average = Text("AverageValue"), status = Text("CommandStatusText") });
        var image = new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth), (int)Math.Ceiling(window.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        image.Render(window);
        var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(image));
        using var stream = File.Create(Path.Combine(output, name + ".png")); encoder.Save(stream);
    }
    private static void DisplayCases()
    {
        var firstAlert = new UiAlert { Metric = UiMetric.Cpu, Average = 41, Upper = 10,
            Start = Now.AddMinutes(-4), End = Now.AddMinutes(-3), Reason = "first CPU evidence" };
        var secondAlert = new UiAlert { Metric = UiMetric.Ram, Average = 820e6, Upper = 400e6,
            Start = Now.AddMinutes(-8), End = Now.AddMinutes(-7), Reason = "second RAM evidence" };
        var first = Row("first", 11, 100e6) with { Alert = firstAlert, Alerts = [firstAlert] };
        var second = Row("second", 22, 200e6) with { Alert = secondAlert, Alerts = [secondAlert] };
        var frame = Frame([first, second]);
        window.RenderSnapshot(frame);
        Check(Members().Length == 0 && List.Items.Count == 1, "duplicate group initially remains collapsed");
        window.ExpandAll(true); // 수정 전에는 같은 ViewListRow가 두 번 들어가 Move(index, 2)가 예외를 낸다.
        var visible = Members();
        Check(visible.Length == 2 && List.Items.Count == 3, "expanded duplicate tuple preserves both process rows and group", ObserveList());
        Check(visible.Select(i => Property<string>(i, "Name")).SequenceEqual(["first", "second"]), "duplicate names remain separate");
        Check(Property<string>(visible[0], "Cpu") == "11%" && Property<string>(visible[1], "Cpu") == "22%" &&
            Property<string>(visible[0], "Ram") == "1%" && Property<string>(visible[1], "Ram") == "2%" &&
            Property<string>(visible[0], "Read") == "1.00" && Property<string>(visible[1], "Read") == "2.00" &&
            Property<string>(visible[0], "Write") == "3.00" && Property<string>(visible[1], "Write") == "4.00",
            "both duplicate rows display their own CPU RAM read and write", ObserveList());
        Check(!ReferenceEquals(visible[0], visible[1]) && first.Key == "123:456" && second.Key == "123:456",
            "display differentiation does not alter actual process identity");
        Select(first);
        Check(Text("CurrentValue") == "11%" && Text("AverageValue") == "41%" && Text("AnalysisTarget").Contains("first"),
            "first selection has first current value and CPU evidence");
        Select(second);
        Check(Text("CurrentValue") == "0.20 GB" && Text("AverageValue") == "0.82 GB" && Text("AnalysisTarget").Contains("second"),
            "second selection has second current value and RAM evidence");
        Check(!window.SelectProcess(first.Key), "tuple-only navigation rejects two uncertain rows");
        SafeSelection(second, "rejected tuple navigation cannot retarget to first");
        Capture("duplicate-two-rows");
        window.ExpandAll(false);
        Check(Members().Length == 0 && List.Items.Count == 1, "collapse hides both duplicate members without dropping input");
        window.ExpandAll(true);
        Check(Members().Length == 2 && window.CurrentSnapshot.Rows.Count == 2, "re-expand restores both rows");
        SafeSelection(second, "collapse and expand cannot retarget uncertain selection");
        ((TextBox)window.FindName("SearchBox")).Text = "first";
        Check(Members().Length == 1 && Property<string>(Members()[0], "Name") == "first", "search filters first without aliasing second");
        SafeSelection(second, "search does not attach the visible first row to selected second");
        ((TextBox)window.FindName("SearchBox")).Clear();
        ((ComboBox)window.FindName("SortBox")).SelectedIndex = 1;
        Check(Members().Select(i => Property<string>(i, "Name")).SequenceEqual(["second", "first"]), "CPU sort moves both independent rows");
        Select(second);
        window.RenderSnapshot(frame with { Rows = [second, first] });
        SafeSelection(second, "same-round reorder preserves exact selected object or safely clears");
        Check(Members().Length == 2, "same-round reorder keeps both duplicate rows");
        ((ComboBox)window.FindName("SortBox")).SelectedIndex = 0;
        window.RenderSnapshot(frame with { Rows = [second, first] });
        Check(Members().Select(i => Property<string>(i, "Name")).SequenceEqual(["second", "first"]), "source order reversal remains visible");
        Select(second);
        var firstCopy = first with { CpuPercent = 33 };
        var secondCopy = second with { CpuPercent = 44 };
        window.RenderSnapshot(frame with { Rows = [firstCopy, secondCopy] });
        Check(window.SelectedRow == null && List.SelectedItem == null, "replaced uncertain objects in the same round cannot inherit a selected display slot or highlight");
        Select(secondCopy);
        window.RenderSnapshot(frame with { Sequence = 2, Rows = [secondCopy, firstCopy] });
        Check(window.SelectedRow == null && List.SelectedItem == null && Text("AnalysisTarget") == "" && Text("CurrentValue") == "",
            "next round clears duplicate uncertain selection and old detail values");
        Check(Members().Length == 2, "next round still displays both input rows");
    }
    private static void SelectionControls()
    {
        var unique = Row("unique", 33, 300e6, true, 789, 999);
        var frame = Frame([unique], 3);
        window.RenderSnapshot(frame); window.ExpandAll(true);
        Check(window.SelectProcess(unique.Key, UiMetric.Cpu), "unique verified tuple can be navigated");
        var replacement = unique with { CpuPercent = 44 };
        window.RenderSnapshot(frame with { Rows = [replacement] });
        Check(ReferenceEquals(window.SelectedRow, replacement) && Text("CurrentValue") == "44%", "unique verified selection follows same-round update");
        replacement = unique with { CpuPercent = 55 };
        window.RenderSnapshot(frame with { Sequence = 4, Rows = [replacement] });
        Check(ReferenceEquals(window.SelectedRow, replacement) && Text("CurrentValue") == "55%", "unique verified selection follows next round");
        var reused = unique with { Creation = 1000, Name = "reused PID", CpuPercent = 66 };
        window.RenderSnapshot(frame with { Sequence = 5, Rows = [reused] });
        Check(window.SelectedRow == null && List.SelectedItem == null && Text("CurrentValue") == "" && !window.SelectProcess(unique.Key), "PID reuse clears old instance and highlight and rejects old key");
        Check(window.SelectProcess(reused.Key, UiMetric.Cpu) && Text("CurrentValue") == "66%", "PID reuse new identity can be selected independently");
        window.RenderSnapshot(frame with { SessionEpoch = 11, Rows = [reused] });
        Check(window.SelectedRow == null && List.SelectedItem == null && window.SelectedProcessKey == null && Text("AnalysisTarget") == "", "new session clears even a verified same-key selection and highlight");
        var uncertain = Row("single uncertain", 12, 120e6);
        frame = Frame([uncertain], 7, 12); window.RenderSnapshot(frame); window.ExpandAll(true); Select(uncertain); SelectMetric(UiMetric.Cpu);
        window.RenderSnapshot(frame);
        Check(ReferenceEquals(window.SelectedRow, uncertain) && Text("CurrentValue") == "12%", "existing single uncertain row is inspectable in the same round");
        int cleared = 0;
        Action<UiRow?> selectionNotice = row => { if (row == null) ++cleared; };
        window.ProcessSelectionChanged += selectionNotice;
        window.RenderSnapshot(frame with { Sequence = 8 });
        Check(window.SelectedRow == null && List.SelectedItem == null && Text("CurrentValue") == "", "single uncertain row selection and highlight expire at next round");
        window.RenderSnapshot(frame with { Sequence = 8 }); window.RenderSnapshot(frame with { Sequence = 8 });
        window.ProcessSelectionChanged -= selectionNotice;
        Check(cleared == 1, "selection invalidation notifies once and unselected redraws do not repeat notification", cleared);
        var valid = Row("verified duplicate", 15, 150e6, true);
        var other = valid with { Name = "uncertain duplicate", IdentityValid = false, CpuPercent = 25 };
        frame = Frame([valid, other], 9, 12); window.RenderSnapshot(frame); window.ExpandAll(true);
        Check(Members().Length == 2 && !window.SelectProcess(valid.Key), "mixed verified and uncertain tuple remains ambiguous for navigation");
        Select(valid); SelectMetric(UiMetric.Cpu);
        Check(Text("CurrentValue") == "15%", "mixed tuple verified row is still individually inspectable");
        window.RenderSnapshot(frame with { Rows = [other, valid] });
        SafeSelection(valid, "mixed tuple reordering does not attach uncertain neighbor");
        window.RenderSnapshot(frame with { Sequence = 10 });
        Check(window.SelectedRow == null && List.SelectedItem == null, "mixed tuple selection and highlight cannot continue as a verified identity next round");
        var otherValid = valid with { Name = "other verified duplicate", CpuPercent = 35 };
        window.RenderSnapshot(frame with { Rows = [valid, otherValid], Sequence = 11 }); window.ExpandAll(true);
        Check(Members().Length == 2 && !window.SelectProcess(valid.Key), "two verified flags do not make a duplicate tuple unique");
        var triple = Frame([valid, other, otherValid], 12, 12);
        window.RenderSnapshot(triple); window.ExpandAll(true);
        Check(Members().Length == 3 && List.Items.Count == 4, "three duplicate rows also occupy independent display slots");
    }
    private static void CreateController()
    {
        var assembly = typeof(MainWindow).Assembly;
        object options = assembly.GetType("PcMonitor.Options", true)!.GetMethod("Parse")!.Invoke(null, [new[] {
            "--synthetic", "--db", Path.Combine(output, "commands.sqlite"), "--interval-ms", "60000",
            "--diagnostics", Path.Combine(output, "worker.jsonl") }])!;
        controllerType = assembly.GetType("PcMonitor.WindowController", true)!;
        controller = Activator.CreateInstance(controllerType, window, options)!;
        loop = controllerType.GetField("loop", Private)!.GetValue(controller)!; loopType = loop.GetType();
        // Show/SourceInitialized는 controller 연결 전에 끝냈다. 실제 worker는 명령 시험에서만
        // 시작하며, 주입한 표시 복사본을 native의 원래 단일 행이 덮지 않도록 발행만 관찰한다.
        var field = loopType.GetField("Published", Private)!;
        var original = (Delegate?)field.GetValue(loop);
        if (original != null) foreach (var d in original.GetInvocationList()) loopType.GetEvent("Published")!.RemoveEventHandler(loop, d);
        loopType.GetEvent("Published")!.AddEventHandler(loop, new Action<UiSnapshot>(s => Volatile.Write(ref published, s)));
    }
    private static NotificationEvent Event(UiSnapshot frame, UiRow row)
    {
        var e = new NotificationEvidence((ulong)Now.AddMinutes(-13).ToFileTime(), (ulong)Now.AddMinutes(-12).ToFileTime(), 52, 5, 12, 60, 3, 0, 1, 0);
        return new(1, (uint)frame.SessionEpoch, long.Parse(row.ProgramId), row.Name, 1, 0,
            [new(new(row.Pid, row.Creation), 1, [e, e, e, e], 0)], default);
    }
    private static void NotificationCases()
    {
        CreateController();
        foreach (var flags in new[] { (false, false), (true, false), (true, true) }) {
            var first = Row("first click", 11, 100e6, flags.Item1);
            var second = Row("second click", 22, 200e6, flags.Item2);
            var frame = Frame([first, second], 20, 30); window.RenderSnapshot(frame); window.ExpandAll(true); Select(second); SelectMetric(UiMetric.Cpu);
            var ev = Event(frame, first);
            controllerType.GetMethod("Navigate", Private)!.Invoke(controller, [ev, 0, 0]);
            SafeSelection(second, "ambiguous click cannot retarget second under identity flags " + flags);
            Check(!Text("AnalysisTarget").Contains("first click") && Text("AverageValue") != "52%" &&
                Text("CommandStatusText").Contains("확인"), "ambiguous notification click is rejected without copying foreign evidence " + flags,
                new { target = Text("AnalysisTarget"), average = Text("AverageValue"), status = Text("CommandStatusText") });
            var clicked = typeof(UiRow).GetProperty("ClickedAlerts", Private)!;
            Check(window.CurrentSnapshot.Rows.All(r => ((IReadOnlyList<UiAlert>)clicked.GetValue(r)!).Count == 0),
                "ambiguous notification leaves all clicked evidence empty " + flags);
        }
        var ram = new UiAlert { Metric = UiMetric.Ram, Average = 880e6, Upper = 440e6, Start = Now.AddMinutes(-8), End = Now.AddMinutes(-7) };
        var unique = Row("unique click", 11, 100e6, true, 800, 900) with { Alert = ram, Alerts = [ram] };
        var control = Frame([unique], 21, 30); window.RenderSnapshot(control);
        controllerType.GetMethod("Navigate", Private)!.Invoke(controller, [Event(control, unique), 0, 0]);
        Check(window.SelectedRow?.Key == unique.Key && Text("AverageValue") == "52%" && Text("UpperValue") == "12%" && Text("CurrentValue") == "11%",
            "unique notification click still preserves clicked CPU values and current sample");
        SelectMetric(UiMetric.Ram);
        Check(Text("AverageTitle") == "당시 구간 평균" && Text("AverageValue") == "0.88 GB", "unique clicked CPU keeps RAM own accepted evidence");
        window.RenderSnapshot(control with { SessionEpoch = 31 });
        controllerType.GetMethod("Navigate", Private)!.Invoke(controller, [Event(control, unique), 0, 0]);
        Check(window.SelectedRow == null && Text("CurrentValue") == "" && Text("CommandStatusText").Contains("세션"), "old-session notification cannot restore a cleared selection");
    }
    private static T Worker<T>(Func<Engine, T> work)
    {
        var done = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        loopType.GetMethod("Enqueue", Private)!.Invoke(loop, [new Action(() => {
            try { done.SetResult(work((Engine)loopType.GetField("engine", Private)!.GetValue(loop)!)); }
            catch (Exception e) { done.SetException(e); }
        })]);
        Until(() => done.Task.IsCompleted, "native worker barrier"); Pump();
        return done.Task.GetAwaiter().GetResult();
    }
    private static Snapshot ReadNative(Engine engine)
    {
        var handle = (IntPtr)typeof(Engine).GetField("handle", Private)!.GetValue(engine)!;
        return (Snapshot)typeof(Engine).GetMethod("CopyRequired", Private)!.Invoke(engine, [bridge_current(handle)])!;
    }
    private static string Signature(Snapshot s) => JsonSerializer.Serialize(new { s.Session, s.Sequence, s.FeedbackRevision, s.TargetId, s.TargetLabel,
        rows = s.Rows.Select(r => new { r.Key, r.ProgramId, r.Feedback, r.FeedbackExpires }),
        groups = s.Groups.Select(g => new { g.Key, g.Label, g.Members }) });
    private static void Act(string action) => controllerType.GetMethod("Act", Private)!.Invoke(controller, [new UiCommand(action, window.SelectedProcessKey, window.SelectedGroup?.Key)]);
    private static MenuItem GroupItem(string action)
    {
        var menu = (ContextMenu)typeof(MainWindow).GetMethod("MakeGroupMenu", Private)!.Invoke(window, null)!;
        string prefix = action switch { "group-target" => "선택 구성원을 이동 대상으로", "group-move" => "선택 구성원을 대상", "group-detach" => "자동 그룹에서 분리", _ => "자동 추정 소속" };
        return menu.Items.OfType<MenuItem>().Single(i => i.Header.ToString()!.StartsWith(prefix, StringComparison.Ordinal));
    }
    private static void RejectedCommand(string action, UiRow row, bool menu)
    {
        Select(row); var before = Worker(ReadNative); string marker = "await " + action; window.SetCommandStatus(marker);
        string priorStatus = Text("CommandStatusText"); int priorRequests = requests.Count;
        if (menu) {
            var item = GroupItem(action);
            Check(!item.IsEnabled, "ambiguous selected row disables menu " + action);
            item.RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent)); // 늦은 메뉴/자동화는 실제 handler도 통과시킨다.
        } else Act(action);
        var after = Worker(ReadNative);
        Check(Signature(before) == Signature(after), "ambiguous " + action + " cannot change actual C membership target or feedback",
            new { before = Signature(before), after = Signature(after), status = Text("CommandStatusText") });
        Check(Text("CommandStatusText") != priorStatus && Text("CommandStatusText").Contains("유일하게 확인") &&
            !Text("CommandStatusText").Contains("저장했습니다") && !Text("CommandStatusText").Contains("이번 세션의 그룹 지정을 적용"),
            "ambiguous " + action + " produces a rejection response", Text("CommandStatusText"));
        if (menu) Check(requests.Count == priorRequests, "ambiguous menu " + action + " emits no process command", new { priorRequests, after = requests.Count });
    }
    private static void RecentCacheCases(UiSnapshot actual)
    {
        var selected = actual.Rows.Single() with { RecentComparisons = [] };
        actual = actual with { Rows = [selected] };
        var cache = new UiRecentComparison[] { new() { Metric = UiMetric.Cpu, State = RecentComparisonState.Ready,
            Count = 5, Average = 66, Median = 33, Start = Now.AddMinutes(-1), End = Now, Duration = 60 } };
        var targetField = loopType.GetField("recentTarget", Private)!;
        var cacheField = loopType.GetField("recentComparisons", Private)!;
        void Prime()
        {
            loopType.GetMethod("SelectRecent")!.Invoke(loop, [actual, selected]);
            Worker(_ => { cacheField.SetValue(loop, cache); return 0; });
        }
        UiSnapshot Attach(UiSnapshot input) => Worker(_ => (UiSnapshot)loopType.GetMethod("AttachRecentComparisons", Private)!.Invoke(loop, [input])!);
        Prime();
        var control = Attach(actual).Rows.Single().RecentComparisons;
        Check(control.Count == 1 && control[0].Average == 66 && control[0].Median == 33 && control[0].Count == 5,
            "unique verified row receives its cached recent values", control);
        foreach (var flags in new[] { (false, false), (true, false), (false, true), (true, true) }) {
            Prime();
            var first = selected with { Name = "recent first", IdentityValid = flags.Item1 };
            var second = selected with { Name = "recent second", IdentityValid = flags.Item2 };
            var duplicate = actual with { Rows = [first, second] };
            foreach (var input in new[] { duplicate, duplicate with { Rows = [second, first] } }) {
                var result = Attach(input);
                Check(result.Rows.Count == 2 && result.Rows.All(r => r.RecentComparisons.Count == 0),
                    "cached recent result attaches to neither duplicate in either order " + flags,
                    result.Rows.Select(r => new { r.Name, r.IdentityValid, recent = r.RecentComparisons }));
            }
            loopType.GetMethod("SelectRecent")!.Invoke(loop, [duplicate, first]);
            Check(Worker(_ => targetField.GetValue(loop) == null && ((UiRecentComparison[])cacheField.GetValue(loop)!).Length == 0),
                "ambiguous SelectRecent clears target and cached result " + flags);
        }
        Prime();
        Check(Worker(e => {
            var current = ReadNative(e); var target = targetField.GetValue(loop)!;
            var find = loopType.GetMethod("FindRecentRow", BindingFlags.Static | BindingFlags.NonPublic)!;
            var row = current.Rows.Single();
            bool unique = ReferenceEquals(find.Invoke(null, [current, target]), row);
            var reusedPid = row with { Key = new(row.Key.Pid, row.Key.Creation + 1), Valid = row.Valid & ~ResourceValid.Identity };
            return unique && find.Invoke(null, [current with { Rows = [row, reusedPid] }, target]) == null &&
                find.Invoke(null, [current with { Rows = [reusedPid, row] }, target]) == null;
        }), "native recent target rejects repeated PID with another creation in both orders while accepting unique control");
        loopType.GetMethod("SelectRecent")!.Invoke(loop, [actual, null]); Worker(_ => 0);
    }
    private static void CommandCases(string expectedNative)
    {
        loopType.GetMethod("Start")!.Invoke(loop, null); started = true;
        Until(() => Volatile.Read(ref published)?.Rows.Count == 1, "production native synthetic first frame");
        var actual = Volatile.Read(ref published)!;
        var native = Worker(ReadNative);
        string loadedNative = Process.GetCurrentProcess().Modules.Cast<ProcessModule>().Single(m => m.ModuleName.Equals("MonitorBridge.dll", StringComparison.OrdinalIgnoreCase)).FileName;
        string nativeHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(loadedNative)));
        Check(nativeHash.Equals(expectedNative, StringComparison.OrdinalIgnoreCase), "actual loaded native module matches distribution SHA", new { path = loadedNative, sha256 = nativeHash });
        ledger.Add(new { stage = "production-worker", workerThread = Worker(_ => Environment.CurrentManagedThreadId), uiThread = Environment.CurrentManagedThreadId,
            native = Signature(native), nativeHash, nativePath = loadedNative });
        var original = actual.Rows.Single();
        window.CommandRequested += c => requests.Add(c);
        window.RenderSnapshot(actual); window.ExpandAll(true);
        Check(window.SelectProcess(original.Key, UiMetric.Cpu), "production single synthetic instance is selectable");
        int priorRequests = requests.Count;
        var targetMenu = GroupItem("group-target");
        Check(targetMenu.IsEnabled, "unique native control menu is enabled");
        targetMenu.RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent)); var control = Worker(ReadNative);
        Check(requests.Count == priorRequests + 1 && requests[^1].ProcessKey == original.Key,
            "unique routed menu emits exactly one actual process tuple", requests.LastOrDefault());
        Check(control.TargetId != 0 && control.TargetLabel.Length != 0, "control actual Act native command sets a real group target", Signature(control));
        window.RenderSnapshot(Volatile.Read(ref published)!); window.SelectProcess(original.Key);
        Act("feedback-suppress-hour"); var feedback = Worker(ReadNative);
        Check(feedback.FeedbackRevision > control.FeedbackRevision && feedback.Rows.Single().Feedback == FeedbackKind.Ignore,
            "control actual Act native feedback changes revision and row state", Signature(feedback));
        window.RenderSnapshot(Volatile.Read(ref published)!); window.SelectProcess(original.Key); Act("feedback-clear"); Worker(ReadNative);
        actual = Volatile.Read(ref published)!;
        RecentCacheCases(actual);
        foreach (var flags in new[] { (false, false), (true, false), (true, true) }) {
            var first = actual.Rows.Single() with { Name = "native first", IdentityValid = flags.Item1, CpuPercent = 11 };
            var second = first with { Name = "native second", IdentityValid = flags.Item2, CpuPercent = 22 };
            var duplicate = actual with { Rows = [first, second], Groups = actual.Groups.Select(g => g with { MemberCount = 2 }).ToArray() };
            window.RenderSnapshot(duplicate); window.ExpandAll(true);
            Check(Members().Length == 2, "native-key duplicate input still displays both rows " + flags);
            foreach (var row in new[] { first, second }) {
                foreach (var action in new[] { "group-target", "group-move", "group-detach", "group-auto" }) RejectedCommand(action, row, true);
                foreach (var action in new[] { "feedback-suppress-hour", "feedback-expected-day", "feedback-clear" }) RejectedCommand(action, row, false);
            }
            Capture("commands-" + (flags.Item1 ? "valid" : "uncertain") + "-" + (flags.Item2 ? "valid" : "uncertain"));
        }
        Check(requests.Count > 0 && requests.All(c => c.ProcessKey == null || c.ProcessKey == original.Key),
            "actual menu requests never use a temporary display key as process identity", requests);
        var stale = actual with { SessionEpoch = actual.SessionEpoch + 100 };
        window.RenderSnapshot(stale); window.SelectProcess(original.Key);
        var beforeStale = Worker(ReadNative); Act("group-target"); var afterStale = Worker(ReadNative);
        Check(Signature(beforeStale) == Signature(afterStale) && Text("CommandStatusText").Contains("세션"), "stale session command is rejected by real worker without native mutation");
    }
    [STAThread] private static int Main(string[] args)
    {
        if (args.Length != 3) return 2;
        output = Path.GetFullPath(args[0]); if (Directory.Exists(output)) return 2; Directory.CreateDirectory(output);
        CultureInfo.CurrentCulture = CultureInfo.GetCultureInfo("ko-KR");
        CultureInfo.CurrentUICulture = CultureInfo.GetCultureInfo("ko-KR");
        string assemblyPath = typeof(MainWindow).Assembly.Location;
        string managedHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(assemblyPath)));
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        try {
            Check(managedHash.Equals(args[1], StringComparison.OrdinalIgnoreCase), "actual loaded managed assembly matches distribution SHA", new { path = assemblyPath, sha256 = managedHash });
            window = new MainWindow { Width = 1440, Height = 900, ShowActivated = false, ShowInTaskbar = false, Left = -32000, Top = -32000 };
            window.Show();
            Scenario("duplicate display", DisplayCases);
            Scenario("selection controls", SelectionControls);
            Scenario("notification identity boundary", NotificationCases);
            if (loop != null) Scenario("actual worker command boundary", () => CommandCases(args[2]));
        } catch (Exception e) { Check(false, "runner setup", e.ToString()); }
        finally {
            if (loop != null && started) {
                try { window.Close(); Until(() => (bool)controllerType.GetField("finished", Private)!.GetValue(controller)!, "production worker shutdown"); }
                catch (Exception e) { Check(false, "worker shutdown", e.ToString()); }
            } else if (window != null) {
                if (controller != null) controllerType.GetField("finished", Private)!.SetValue(controller, true);
                window.Close();
                if (loop != null) loopType.GetMethod("ReleaseWakeHandle")!.Invoke(loop, null);
            }
        }
        File.WriteAllText(Path.Combine(output, "results.json"), JsonSerializer.Serialize(new {
            checks = checks.Count, failures, managedSha256 = managedHash, expectedNativeSha256 = args[2], results = checks, ledger,
            syntheticUiSnapshots = true, actualProductAssembly = true, actualNativeWorker = started,
            publishedFramesCapturedWithoutReplacingInjectedUi = true, actualShellClick = false,
            actualExternalLookup = false, realSystemCollection = false
        }, Json));
        Console.WriteLine($"duplicate rows checks={checks.Count} failures={failures}; results: {output}");
        return failures == 0 ? 0 : 1;
    }
}
