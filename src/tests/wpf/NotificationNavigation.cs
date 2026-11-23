// Shell 송출 결과와 클릭 콜백만 모사하는 통합 시험이다. Native 정책·신원 재확인,
// worker, controller, WPF는 전달한 제품 DLL을 그대로 사용한다. 송출 payload의
// 당시 평균·상한·구간 시각이 클릭 뒤 카드에 남는지 확인하며 실제 배너 클릭은 아니다.
using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using PcMonitor;

internal static class NotificationNavigation
{
    private static readonly BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    [STAThread]
    private static int Main(string[] args)
    {
        string output = Path.GetFullPath(args.Length == 0 ? "notification-navigation-result.json" : args[0]);
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        var assembly = typeof(MainWindow).Assembly;
        if (!ClickedEvidenceCases(assembly, output + ".evidence.json")) return 1;
        if (args.Contains("--evidence-only")) return 0;
        object options = assembly.GetType("PcMonitor.Options", true)!.GetMethod("Parse")!.Invoke(null,
            new object[] { new[] { "--ui-notification-demo", "--interval-ms", "1000", "--diagnostics", output + ".jsonl" } })!;
        var window = new MainWindow { Width = 1280, Height = 800 };
        Type controllerType = assembly.GetType("PcMonitor.WindowController", true)!;
        object controller = Activator.CreateInstance(controllerType, window, options)!;
        object loop = controllerType.GetField("loop", Private)!.GetValue(controller)!;
        Type loopType = loop.GetType();
        ulong dispatchedId = 0;
        NativeNotification dispatched = default;
        long dispatchedAt = 0;
        long injectedAt = 0;
        bool injected = false, clickQueued = false, finished = false;
        int result = 1;
        var clock = Stopwatch.StartNew();
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        var shell = new Func<NativeNotification, TrayRequestResult>(value =>
        {
            dispatchedId = value.Id;
            dispatched = value;
            dispatchedAt = clock.ElapsedMilliseconds;
            return TrayRequestResult.Requested; // Shell IPC 대신 요청 수락만 돌려주는 명시적 시험 대체점이다.
        });
        loopType.GetField("RequestShell")!.SetValue(loop, shell);
        string Text(string name) => ((TextBlock)window.FindName(name)).Text;
        void Finish(object evidence, bool success)
        {
            if (finished) return;
            finished = true;
            timer.Stop();
            File.WriteAllText(output, JsonSerializer.Serialize(evidence, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine(success ? "PASS simulated ACK/callback -> real C policy and WPF navigation" : "FAIL notification navigation; see result JSON");
            result = success ? 0 : 1;
            window.Close(); // 제품 Closing 경로가 worker와 native engine 종료를 기다린다.
        }
        timer.Tick += (_, _) =>
        {
            try
            {
                if (!injected && window.CurrentSnapshot.Rows.Count == 1)
                {
                    injected = true;
                    injectedAt = clock.ElapsedMilliseconds;
                    controllerType.GetMethod("Act", Private)!.Invoke(controller, new object[] { new UiCommand("demo-notification") });
                }
                if (dispatchedId != 0 && !clickQueued && clock.ElapsedMilliseconds - dispatchedAt >= 200)
                {
                    clickQueued = true;
                    // TrayHost가 NIN_BALLOONUSERCLICK 뒤 호출하는 같은 controller 진입점이다.
                    controllerType.GetMethod("OnTray", Private)!.Invoke(controller, new object[] { TrayAction.Click, dispatchedId });
                }
                if (clickQueued && window.SelectedRow is { } row && Text("AnalysisTarget").Contains("CPU 알림"))
                {
                    using var self = Process.GetCurrentProcess();
                    var creation = (ulong)self.StartTime.ToUniversalTime().ToFileTimeUtc();
                    bool identity = row.Pid == (uint)Environment.ProcessId && row.Creation == creation && row.IdentityValid;
                    bool cause = window.SelectedMetric == UiMetric.Cpu;
                    bool values = Text("AverageValue") == "52%" && Text("UpperValue") == "12%" && Text("CurrentValue") == "2%";
                    bool delay = dispatchedAt - injectedAt >= 14500;
                    bool labels = Text("AverageTitle") == "당시 구간 평균" && Text("UpperTitle") == "당시 평소 상한";
                    // 52%-12% 차이는 상대 증가율이 아니라 40%p다.
                    bool reasonUnits = !Text("AnalysisReason").Contains("상한보다 40% 높");
                    var saved = dispatched.Members![0].Metric[0];
                    var start = DateTimeOffset.FromFileTime((long)saved.Start).ToLocalTime();
                    var end = DateTimeOffset.FromFileTime((long)saved.End).ToLocalTime();
                    bool times = Text("AverageTime") == $"{start:MM-dd HH:mm:ss}–{end:HH:mm:ss}" &&
                        Text("UpperTime") == $"{end:HH:mm:ss} 기준" &&
                        Text("CurrentTime") == $"{row.ObservedAt!.Value.ToLocalTime():HH:mm:ss} 측정";
                    // 수락 원본은 그대로이고 클릭 사건은 별도 우선한다. 기존 단일 사건 경로의
                    // 당시 값은 카드와 원본에서 각각 대조해 현재 표본이 끼어들지 않게 한다.
                    bool savedValues = row.Alert is { } accepted && accepted.Average == saved.Current && accepted.Upper == saved.Threshold &&
                        accepted.Start == start && accepted.End == end;
                    Finish(new { simulatedShellAcknowledgment = true, simulatedControllerClick = true,
                        actualWindowsShellClick = false, dispatchedId, injectedAt, dispatchedAt,
                        policyDelayMs = dispatchedAt - injectedAt, identity, cause, values, delay, labels, reasonUnits,times,savedValues,
                        savedStart = saved.Start, savedEnd = saved.End, savedAverage = saved.Current, savedUpper = saved.Threshold,
                        processId = Environment.ProcessId, expectedCreation = creation, row.Pid, row.Creation,
                        row.Key, window.SelectedProcessKey, window.SelectedMetric,
                        target = Text("AnalysisTarget"), reason = Text("AnalysisReason"),
                        average = Text("AverageValue"), upper = Text("UpperValue"), current = Text("CurrentValue"),
                        averageTime = Text("AverageTime"), upperTime = Text("UpperTime"), currentTime = Text("CurrentTime"),
                        commandStatus = Text("CommandStatusText") }, identity && cause && values && delay && labels && reasonUnits && times && savedValues);
                }
                else if (clock.ElapsedMilliseconds > 35000)
                    Finish(new { error = "timeout", injected, clickQueued, dispatchedId,
                        commandStatus = Text("CommandStatusText"), selected = window.SelectedProcessKey }, false);
            }
            catch (Exception error) { Finish(new { error = error.ToString() }, false); }
        };
        window.Loaded += (_, _) => timer.Start();
        app.Run(window);
        return result;
    }

    // 감사 B1과 같은 controller 표시 진입점에 고정 사본을 넣고 실제 카드·목록·요약을
    // 단언한다. native 수락/억제 정책은 위 통합 경로와 C 정책 회귀가 따로 확인한다.
    // 이 입력은 수정 전 제품 DLL에도 그대로 실행할 수 있어 종료 0인 관찰 probe를 보완한다.
    private static bool ClickedEvidenceCases(Assembly assembly, string output)
    {
        var checks = new List<object>();
        int failures = 0;
        void Check(bool ok, string name, object? detail = null) { checks.Add(new { name, passed = ok, detail }); if (!ok) ++failures; }
        var w = new MainWindow();
        var options = assembly.GetType("PcMonitor.Options", true)!.GetMethod("Parse")!.Invoke(null,
            new object[] { new[] { "--ui-notification-demo" } })!;
        var controllerType = assembly.GetType("PcMonitor.WindowController", true)!;
        var controller = Activator.CreateInstance(controllerType, w, options)!;
        var clicked = controllerType.GetField("clickedMember", Private)!;
        var last = controllerType.GetField("lastClick", Private)!;
        var withClick = controllerType.GetMethod("WithClickedEvidence", Private)!;
        string Text(string name) => ((TextBlock)w.FindName(name)).Text;
        string Window(DateTimeOffset start, DateTimeOffset end) => $"{start.ToLocalTime():MM-dd HH:mm:ss}–{end.ToLocalTime():HH:mm:ss}";
        var now = new DateTimeOffset(2026, 11, 20, 12, 30, 0, TimeSpan.Zero);
        UiAlert Saved(UiMetric metric, double value, double? upper, int minutes) => new() {
            Metric = metric, Average = value, Upper = upper, Start = now.AddMinutes(minutes - 1), End = now.AddMinutes(minutes),
            AcceptedAt = now.AddMinutes(minutes).AddSeconds(5), Reason = "accepted " + metric
        };
        var cpu = Saved(UiMetric.Cpu, 70, 35, -3);
        var ram = Saved(UiMetric.Ram, 880e6, 440e6, -7);
        var row = new UiRow { Pid = 123, Creation = 456, ProgramId = "5", Name = "근거 시험", Path = "SYNTHETIC",
            GroupKey = "fixture", CpuPercent = 7, WorkingSetBytes = 80e6, ObservedAt = now,
            Alert = cpu, Alerts = [cpu, ram], Comparisons = [new UiComparison { Metric = UiMetric.Cpu, StateCode = MetricState.Normal,
                Average = 9, Upper = 45, Start = now.AddMinutes(-1), End = now }, new UiComparison { Metric = UiMetric.Ram,
                StateCode = MetricState.Normal, Average = 111e6, Upper = 222e6, Start = now.AddMinutes(-1), End = now }] };
        var group = new UiGroup { Key = "fixture", Name = "근거 그룹", MemberCount = 1, AlertCount = 1 };
        var frame = new UiSnapshot { SessionEpoch = 2, Sequence = 1, IsSynthetic = true, ObservedAt = now, Rows = [row], Groups = [group] };
        NotificationEvidence Evidence(double value, double upper, int minutes) => new((ulong)now.AddMinutes(minutes - 1).ToFileTime(),
            (ulong)now.AddMinutes(minutes).ToFileTime(), value, 5, upper, 60, 3, 0, 1, 0);
        NotificationEvent Event(uint metrics, NotificationEvidence? c = null, NotificationEvidence? r = null) =>
            new(1, 2, 5, row.Name, metrics, 0, [new NotificationMember(new ProcessKey(row.Pid, row.Creation), metrics,
                [c ?? Evidence(52, 12, -13), r ?? Evidence(660e6, 330e6, -9), Evidence(0, 0, -5), Evidence(0, 0, -5)], 0)], default);
        UiSnapshot Paint(UiSnapshot input, NotificationEvent? ev = null) {
            if (ev != null) { last.SetValue(controller, ev); clicked.SetValue(controller, 0); }
            return (UiSnapshot)withClick.Invoke(controller, [input])!;
        }
        void Show(UiSnapshot input, UiMetric metric) { w.RenderSnapshot(input); w.SelectProcess(input.Rows[0].Key, metric); }
        string Label() {
            var item = ((ListBox)w.FindName("ProcessList")).Items.Cast<object>().First(i => i.GetType().GetProperty("Row")!.GetValue(i) is UiRow);
            return (string)item.GetType().GetProperty("AlertLabel")!.GetValue(item)!;
        }
        string Summary(UiSnapshot input) => (string)typeof(MainWindow).GetMethod("GroupSummary", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, [group, input.Rows.ToArray()])!;
        try {
            Show(frame, UiMetric.Ram);
            Check(Text("AverageTitle") == "당시 구간 평균" && Text("AverageValue") == "0.88 GB", "accepted RAM evidence exists before CPU click");
            var cpuClick = Paint(frame, Event(1));
            Show(cpuClick, UiMetric.Cpu);
            Check(Text("AverageTitle") == "당시 구간 평균" && Text("AverageValue") == "52%" && Text("UpperValue") == "12%",
                "older clicked CPU overrides a newer accepted CPU as one whole event");
            Check(Text("AverageTime") == Window(now.AddMinutes(-14), now.AddMinutes(-13)) && Text("UpperTime") == $"{now.AddMinutes(-13).ToLocalTime():HH:mm:ss} 기준",
                "clicked CPU uses its own start end and upper timestamp", new { average = Text("AverageTime"), upper = Text("UpperTime") });
            Check(Text("CurrentValue") == "7%" && Text("CurrentTime") == $"{now.ToLocalTime():HH:mm:ss} 측정" && cpuClick.ObservedAt == frame.ObservedAt,
                "current raw CPU and observation time remain independent of click");
            Check(Text("AnalysisTarget") == "근거 시험에 CPU 알림 1건" && Label() == "CPU 알림" && Summary(cpuClick) == "근거 시험에 CPU 알림 1건",
                "other accepted RAM is not counted as this CPU event cause title list or group summary", new { title = Text("AnalysisTarget"), label = Label(), summary = Summary(cpuClick) });
            Check(ReferenceEquals(cpuClick.Rows[0].Alerts, row.Alerts) && ReferenceEquals(cpuClick.Rows[0].Alert, cpu) && row.Alerts.Count == 2,
                "controller preserves accepted evidence source and immutable worker frame");
            Show(cpuClick, UiMetric.Ram);
            Check(Text("AverageTitle") == "당시 구간 평균" && Text("UpperTitle") == "당시 평소 상한" && Text("AverageValue") == "0.88 GB" && Text("UpperValue") == "0.44 GB",
                "CPU click retains RAM accepted average and upper", new { label = Text("AverageTitle"), average = Text("AverageValue"), upper = Text("UpperValue") });
            Check(Text("AverageTime") == Window(ram.Start!.Value, ram.End!.Value) && Text("UpperTime") == $"{ram.End.Value.ToLocalTime():HH:mm:ss} 기준" &&
                Text("CurrentValue") == "0.08 GB" && Text("CurrentTime") == $"{now.ToLocalTime():HH:mm:ss} 측정",
                "RAM accepted interval and current observation timestamps remain distinct", new { interval = Text("AverageTime"), current = Text("CurrentTime") });
            Check(Text("AnalysisTarget") == "근거 시험의 RAM 알림 기록" && Label() == "CPU 알림", "other metric is labeled as its own record rather than the clicked cause", Text("AnalysisTarget"));
            var refreshed = Paint(frame with { Sequence = 2, ObservedAt = now.AddSeconds(2), Rows = [row with {
                CpuPercent = 8, ObservedAt = now.AddSeconds(2), Alerts = [cpu with { Average = 80 }, ram with { Average = 990e6 }] }] });
            Show(refreshed, UiMetric.Cpu);
            Check(Text("AverageValue") == "52%" && Text("UpperValue") == "12%" && Text("CurrentValue") == "8%" &&
                Text("CurrentTime") == $"{now.AddSeconds(2).ToLocalTime():HH:mm:ss} 측정", "new worker frame keeps clicked CPU while refreshing independent raw sample");
            Show(refreshed, UiMetric.Ram);
            Check(Text("AverageValue") == "0.99 GB" && Text("AverageTime") == Window(ram.Start!.Value, ram.End!.Value),
                "new worker frame can update other metric accepted evidence independently");
            var olderClick = Paint(cpuClick, Event(1, Evidence(31, 3, -19)));
            Show(olderClick, UiMetric.Cpu);
            Check(Text("AverageValue") == "31%" && Text("UpperValue") == "3%" && Text("AverageTime") == Window(now.AddMinutes(-20), now.AddMinutes(-19)),
                "another older event of same metric replaces the preceding clicked event");
            var ramClick = Paint(cpuClick, Event(2));
            Show(ramClick, UiMetric.Cpu);
            Check(Text("AverageValue") == "70%" && Text("UpperValue") == "35%" && Text("AverageTime") == Window(cpu.Start!.Value, cpu.End!.Value),
                "next RAM click restores latest accepted CPU instead of resurrecting previous CPU overlay", new { average = Text("AverageValue"), interval = Text("AverageTime") });
            Show(ramClick, UiMetric.Ram);
            Check(Text("AverageValue") == "0.66 GB" && Text("UpperValue") == "0.33 GB" && Label() == "RAM 알림" && Summary(ramClick) == "근거 시험에 RAM 알림 1건",
                "next RAM event supplies only its own saved values and cause");
            var both = Paint(ramClick, Event(3));
            Show(both, UiMetric.Cpu);
            Check(Text("AnalysisTarget") == "근거 시험의 CPU 알림" && Label() == "알림 지표 2개" && Summary(both) == "근거 시험에 알림 · CPU·RAM",
                "multiple metrics from the same event remain multiple genuine causes");
            var partial = Paint(both, Event(1, Evidence(52, double.NaN, -13)));
            Show(partial, UiMetric.Cpu);
            Check(Text("AverageValue") == "52%" && Text("UpperValue") == "근거 없음" && Text("AverageTime") == Window(now.AddMinutes(-14), now.AddMinutes(-13)) &&
                ((Border)w.FindName("AverageCard")).Tag == null && Text("AnalysisTarget") == "근거 시험의 CPU 알림 기록",
                "clicked missing upper never borrows newer accepted or personal upper");
            Show(partial, UiMetric.Ram);
            Check(Text("AverageValue") == "0.88 GB" && Text("UpperValue") == "0.44 GB", "partial clicked CPU preserves independent RAM evidence");
            var zero = Paint(partial, Event(1, Evidence(0, 0, -13)));
            Show(zero, UiMetric.Cpu);
            Check(Text("AverageValue") == "0%" && Text("UpperValue") == "0%" && Text("AnalysisTarget") == "근거 시험에 CPU 알림 1건" &&
                ((Border)w.FindName("AverageCard")).Tag == null, "valid zero clicked average and upper are evidence without exceeded emphasis");
            var noRamUpper = frame with { Rows = [row with { Alerts = [cpu, ram with { Upper = null }] }] };
            Show(Paint(noRamUpper, Event(1)), UiMetric.Ram);
            Check(Text("AverageValue") == "0.88 GB" && Text("UpperValue") == "근거 없음", "other metric missing accepted upper stays missing");
            var zeroRam = ram with { Average = 0, Upper = 0 };
            Show(Paint(frame with { Rows = [row with { Alerts = [cpu, zeroRam] }] }, Event(1)), UiMetric.Ram);
            Check(Text("AverageValue") == "0.00 GB" && Text("UpperValue") == "0.00 GB", "other metric valid accepted zeros remain valid");
            var legacy = frame with { Rows = [row with { Alert = ram, Alerts = [] }] };
            Show(Paint(legacy, Event(1)), UiMetric.Ram);
            Check(Text("AverageValue") == "0.88 GB" && Text("AverageTime") == Window(ram.Start!.Value, ram.End!.Value), "legacy single Alert remains independent accepted evidence");
            Show(Paint(frame with { Rows = [row with { Alert = null }] }, Event(1)), UiMetric.Ram);
            Check(Text("AverageValue") == "0.88 GB", "Alerts-only accepted evidence remains supported");
            Show(Paint(frame, Event(1)), UiMetric.Cpu);
            w.SelectGroup(group.Key); w.SelectProcess(row.Key, UiMetric.Cpu);
            Check(Text("AverageValue") == "70%" && Text("AverageTime") == Window(cpu.Start!.Value, cpu.End!.Value) && (int)clicked.GetValue(controller)! == -1,
                "selection clear immediately discards overlay before another worker frame or reselect");
            Show(Paint(frame, Event(1)), UiMetric.Cpu);
            var reused = Paint(frame with { Rows = [row with { Creation = 457, Alert = null, Alerts = [] }] });
            Show(reused, UiMetric.Cpu);
            Check(Text("AverageValue") == "9%" && Text("AverageTitle") == "최근 구간 평균" && (int)clicked.GetValue(controller)! == -1,
                "PID reuse never receives old instance clicked evidence");
            var changed = Paint(frame with { Rows = [row with { ProgramId = "6", Alert = null, Alerts = [] }] }, Event(1));
            Show(changed, UiMetric.Cpu);
            Check(Text("AverageValue") == "9%" && (int)clicked.GetValue(controller)! == -1, "program change invalidates click even when PID and creation match");
            var session = Paint(frame with { SessionEpoch = 3, Rows = [row with { Alert = null, Alerts = [] }] }, Event(1));
            Show(session, UiMetric.Cpu);
            Check(Text("AverageValue") == "9%" && (int)clicked.GetValue(controller)! == -1, "session change invalidates clicked evidence");
            Paint(frame with { Rows = [row with { IdentityValid = false }] }, Event(1));
            Check((int)clicked.GetValue(controller)! == -1, "unverified identity invalidates clicked evidence");
            Paint(frame with { Rows = [] }, Event(1));
            var returned = Paint(frame);
            Show(returned, UiMetric.Cpu);
            Check(Text("AverageValue") == "70%", "removed target returning later cannot revive a discarded click");
            var duplicate = Paint(frame with { Rows = [row, row] }, Event(1));
            Check(duplicate.Rows.All(r => r.Alert == cpu && ReferenceEquals(r.Alerts, row.Alerts)) && (int)clicked.GetValue(controller)! == -1,
                "ambiguous duplicate identity receives no clicked overlay");
        } catch (Exception e) { Check(false, "unexpected evidence regression exception", e.ToString()); }
        finally {
            var loop = controllerType.GetField("loop", Private)!.GetValue(controller)!;
            controllerType.GetField("finished", Private)!.SetValue(controller, true);
            loop.GetType().GetMethod("ReleaseWakeHandle")!.Invoke(loop, null); // 이 합성 표시 창은 worker를 시작하지 않았다.
            w.Close(); // Application 종료 때 숨김 시험 창이 해제한 worker wake에 다시 Stop을 보내지 않는다.
        }
        File.WriteAllText(output, JsonSerializer.Serialize(new { checks = checks.Count, failures, product = assembly.Location,
            sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(assembly.Location))), actualCards = true,
            simulatedEvent = true, actualWindowsShellClick = false, results = checks }, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"clicked evidence checks={checks.Count} failures={failures}");
        return failures == 0;
    }
}
