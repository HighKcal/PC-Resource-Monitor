// 배포 DLL의 실제 synthetic worker→controller→창과 native 이동 대상을 함께 검사한다.
// X/Y 결과·오류·갱신 대기 frame은 명시한 합성 주입이며 실제 실패 주입과 구분한다.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal static class SessionGroupStatus
{
    private const BindingFlags Private = BindingFlags.Instance | BindingFlags.NonPublic;
    private const string Applied = "이번 세션의 그룹 지정을 적용했습니다.";
    private const string X = Applied + " 이동 대상 · X";
    private const string Y = Applied + " 이동 대상 · Y";
    private const string Saved = "이 프로그램 경로의 알림 설정을 저장했습니다.";
    private const string Pending = "목록 갱신을 기다리는 중입니다.";
    private const string Retention = "오래된 기록 정리 지연";
    // pg_target_label은 대상 없음의 표시값으로 빈 문자열 대신 "none"을 반환한다.
    private const string NoTargetLabel = "none";
    private static readonly List<object> checks = new(), ledger = new();
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    private static string output = "";
    private static MainWindow window = null!;
    private static object loop = null!, controller = null!;
    private static Type loopType = null!, controllerType = null!;
    private static int failures;

    private static void Check(bool passed, string name, object? detail = null)
    {
        checks.Add(new { name, passed, detail });
        if (!passed) { ++failures; Console.Error.WriteLine("FAIL " + name); }
    }
    private static void Require(bool passed, string name)
    {
        Check(passed, name);
        if (!passed) throw new InvalidOperationException(name);
    }
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    private static string Status => ((TextBlock)window.FindName("CommandStatusText")).Text;
    private static void Until(Func<bool> ready, string name, int milliseconds = 10000)
    {
        var elapsed = Stopwatch.StartNew();
        while (!ready()) {
            if (elapsed.ElapsedMilliseconds >= milliseconds) throw new TimeoutException(name);
            var frame = new DispatcherFrame();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(10) };
            timer.Tick += (_, _) => { timer.Stop(); frame.Continue = false; };
            timer.Start(); Dispatcher.PushFrame(frame);
        }
    }
    private static T Worker<T>(Func<Engine, T> action)
    {
        var done = new TaskCompletionSource<T>();
        loopType.GetMethod("Enqueue", Private)!.Invoke(loop, [new Action(() => {
            try { done.SetResult(action((Engine)loopType.GetField("engine", Private)!.GetValue(loop)!)); }
            catch (Exception e) { done.SetException(e); }
        })]);
        Until(() => done.Task.IsCompleted, "worker observation completed");
        return done.Task.GetAwaiter().GetResult();
    }
    private static Snapshot Native(string stage)
    {
        var native = Worker(e => e.Current ?? throw new InvalidOperationException("Missing native snapshot"));
        ledger.Add(new { stage, source = "production Engine.Current copy on its worker", native.Session,
            native.Sequence, native.TargetId, native.TargetLabel, native.Paused, native.GroupRefreshPending,
            native.StorageError, native.CollectorError });
        return native;
    }
    private static void Act(string name) => controllerType.GetMethod("Act", Private)!.Invoke(controller, [new UiCommand(name)]);
    private static void NextSample()
    {
        long sequence = window.CurrentSnapshot.Sequence;
        Worker(e => { loopType.GetField("nextSample", Private)!.SetValue(loop, 0L); return 0; });
        Until(() => window.CurrentSnapshot.Sequence > sequence, "next actual native synthetic sample displayed");
    }
    private static void GroupResult(uint session, string text)
    {
        ledger.Add(new { stage = "injected-group-result", session, text,
            source = "synthetic result through production GroupStatus event and controller dispatcher" });
        ((Action<uint, string>)loopType.GetField("GroupStatus", Private)!.GetValue(loop)!).Invoke(session, text);
        bool flushed = false;
        window.Dispatcher.BeginInvoke(new Action(() => flushed = true));
        Until(() => flushed, "injected group callback dispatched");
    }
    private static void Publish(UiSnapshot frame, string reason)
    {
        ledger.Add(new { stage = "injected-frame", reason, frame.SessionEpoch, frame.Sequence,
            frame.Status, frame.CommandStatus, frame.RetentionPending, frame.GroupRefreshPending });
        controllerType.GetMethod("Publish", Private)!.Invoke(controller, [frame]);
        controllerType.GetMethod("Consume", Private)!.Invoke(controller, null);
    }
    private static void Capture(string stage)
    {
        window.UpdateLayout(); var s = window.CurrentSnapshot;
        ledger.Add(new { stage = "screen-" + stage, s.SessionEpoch, s.Sequence, s.Paused,
            s.Status, s.RetentionPending, s.GroupRefreshPending, commandStatus = Status,
            selectedKey = window.SelectedProcessKey, selectedRow = window.SelectedRow?.Name,
            selectedGroup = window.SelectedGroup?.Name });
        var image = new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth), (int)Math.Ceiling(window.ActualHeight), 96, 96, PixelFormats.Pbgra32);
        image.Render(window); var png = new PngBitmapEncoder(); png.Frames.Add(BitmapFrame.Create(image));
        using var stream = File.Create(Path.Combine(output, stage + ".png")); png.Save(stream);
    }
    private static void SelectRow(string stage) => Require(window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key, UiMetric.Cpu), stage);
    private static void NewSession()
    {
        long session = window.CurrentSnapshot.SessionEpoch;
        Act("pause"); Until(() => window.CurrentSnapshot.Paused, "pause displayed");
        Act("resume"); Until(() => window.CurrentSnapshot.SessionEpoch != session && window.CurrentSnapshot.Rows.Count == 1, "first new-session native frame displayed");
    }

    private static void Cases(string expectedNative)
    {
        Until(() => window.CurrentSnapshot.Rows.Count == 1, "initial persisted-synthetic native frame");
        var modules = Process.GetCurrentProcess().Modules.Cast<ProcessModule>()
            .Where(m => string.Equals(m.ModuleName, "MonitorBridge.dll", StringComparison.OrdinalIgnoreCase))
            .Select(m => new { path = m.FileName!, sha256 = Hash(m.FileName!) }).ToArray();
        Check(modules.Length == 1 && modules[0].sha256.Equals(expectedNative, StringComparison.OrdinalIgnoreCase),
            "actually loaded native module matches production SHA256", modules);
        SelectRow("session A selects known native instance");
        Act("group-target"); Until(() => Status.Contains(Applied, StringComparison.Ordinal), "actual session A group target response");
        var a = Native("actual-target-X-in-A");
        Require(a.TargetId != 0 && a.TargetLabel.Length > 0, "session A actual native target exists");
        Check(Status.Contains("이동 대상 · " + a.TargetLabel, StringComparison.Ordinal), "actual A response names actual native target", Status);
        string actualAStatus = Status;
        NextSample(); var refreshedA = Native("same-session-A-refresh");
        Check(refreshedA.Session == a.Session && refreshedA.TargetId == a.TargetId && refreshedA.TargetLabel == a.TargetLabel && Status == actualAStatus,
            "same-session actual sample preserves valid target and its acknowledgement", Status);

        // 생산 synthetic 입력은 같은 이름의 프로세스 하나다. 실제 native 대상 확인 뒤
        // X/Y를 이벤트에 넣어 문자열의 세션 수명을 별도로 단언한다.
        GroupResult(a.Session, X); Capture("session-A-target-X");
        Require(Status.Contains("이동 대상 · X", StringComparison.Ordinal), "same-session synthetic X response accepted");
        Act("pause"); Until(() => window.CurrentSnapshot.Paused, "A paused before resume");
        var paused = Native("actual-A-paused");
        Check(paused.TargetId == a.TargetId && paused.TargetLabel == a.TargetLabel && Status.Contains("이동 대상 · X", StringComparison.Ordinal),
            "pause preserves current-session target and valid group acknowledgement", Status);
        Act("resume"); Until(() => window.CurrentSnapshot.SessionEpoch != a.Session && window.CurrentSnapshot.Rows.Count == 1, "first session B snapshot");
        // 다음 타이머/명령 전에 첫 화면의 실제 문자열을 고정한다.
        string firstBStatus = Status; Capture("first-session-B");
        var b = Native("first-B-native-target-reset");
        Check(b.Session != a.Session && b.TargetId == 0 && b.TargetLabel == NoTargetLabel,
            "first session B native snapshot has no old target", new { b.Session, b.TargetId, b.TargetLabel });
        Check(!firstBStatus.Contains("이동 대상", StringComparison.Ordinal) && !firstBStatus.Contains(Applied, StringComparison.Ordinal),
            "first session B screen removes session A target acknowledgement", firstBStatus);
        Check(window.SelectedProcessKey == null && window.SelectedRow == null && window.SelectedGroup == null,
            "first session B clears the former selected process and group");

        // primary regression 결과를 이미 고정했다. 이후 검사는 독립적인 늦은 응답과
        // 새 세션의 유효한 결과를 같은 runner에서 모두 확인한다.
        window.SetCommandStatus("");
        GroupResult(a.Session, X);
        Check(!Status.Contains("이동 대상 · X", StringComparison.Ordinal), "late A response cannot resurrect X in B", Status);
        SelectRow("session B selects its current native instance");
        Act("group-target"); Until(() => Status.Contains(Applied, StringComparison.Ordinal), "actual B group target response");
        var by = Native("actual-target-Y-in-B");
        Check(by.Session == b.Session && by.TargetId != 0 && by.TargetLabel.Length > 0 &&
            Status.Contains("이동 대상 · " + by.TargetLabel, StringComparison.Ordinal), "B can create and show its actual native target", Status);
        GroupResult(by.Session, Y); Capture("session-B-target-Y");
        Check(Status.Contains("이동 대상 · Y", StringComparison.Ordinal), "current-session Y response accepted", Status);
        NextSample(); var refreshedB = Native("same-session-B-refresh");
        Check(refreshedB.TargetId == by.TargetId && refreshedB.TargetLabel == by.TargetLabel && Status.Contains("이동 대상 · Y", StringComparison.Ordinal),
            "normal B sample retains actual target and valid Y acknowledgement", Status);
        GroupResult(a.Session, X);
        Check(Status.Contains("이동 대상 · Y", StringComparison.Ordinal) && !Status.Contains("이동 대상 · X", StringComparison.Ordinal),
            "late A result cannot replace current B target Y acknowledgement", Status);
        GroupResult(a.Session, "");
        Check(Status.Contains("이동 대상 · Y", StringComparison.Ordinal), "late empty A result cannot clear Y", Status);

        Act("feedback-suppress-hour"); Until(() => Status.Contains(Saved, StringComparison.Ordinal), "actual feedback save response displayed");
        var feedback = Native("actual-feedback-save");
        Check(feedback.Rows.Single().Feedback == FeedbackKind.Ignore && !Status.Contains("이동 대상", StringComparison.Ordinal),
            "actual feedback commits and replaces group acknowledgement", Status);
        GroupResult(feedback.Session, "");
        Check(Status.Contains(Saved, StringComparison.Ordinal), "empty current group result preserves independent feedback response", Status);
        NewSession(); var c = Native("actual-session-C-after-feedback");
        Check(c.TargetId == 0 && c.TargetLabel == NoTargetLabel && Status.Contains(Saved, StringComparison.Ordinal),
            "session reset clears actual target while preserving independent feedback response", Status);
        Check(window.SelectedProcessKey == null && window.SelectedRow == null,
            "later session transition also clears selection");
        GroupResult(by.Session, Y);
        Check(Status.Contains(Saved, StringComparison.Ordinal) && !Status.Contains("이동 대상", StringComparison.Ordinal),
            "late B result cannot overwrite C feedback response", Status);

        var current = window.CurrentSnapshot;
        Publish(current with { GroupRefreshPending = true, RetentionPending = true }, "synthetic pending and retention flags only");
        Capture("pending-with-feedback-and-retention");
        Check(Status.Contains(Pending, StringComparison.Ordinal) && Status.Contains(Retention, StringComparison.Ordinal) && Status.Contains(Saved, StringComparison.Ordinal),
            "pending and retention notices coexist with independent feedback response", Status);
        GroupResult(c.Session, "");
        Check(Status.Contains(Pending, StringComparison.Ordinal) && Status.Contains(Retention, StringComparison.Ordinal) && Status.Contains(Saved, StringComparison.Ordinal),
            "pending empty group response does not erase feedback or storage warning", Status);
        Publish(current with { GroupRefreshPending = false, RetentionPending = true }, "synthetic normal group recovery with retention still pending");
        Check(!Status.Contains(Pending, StringComparison.Ordinal) && Status.Contains(Retention, StringComparison.Ordinal) && Status.Contains(Saved, StringComparison.Ordinal),
            "group recovery removes only pending notice and retains feedback plus retention warning", Status);
        Publish(current, "synthetic retention recovery");
        Check(!Status.Contains(Pending, StringComparison.Ordinal) && !Status.Contains(Retention, StringComparison.Ordinal) && Status.Contains(Saved, StringComparison.Ordinal),
            "retention recovery removes only storage warning", Status);

        const string storageError = "저장 실패 · synthetic storage-error response";
        Publish(current with { CommandStatus = storageError, Status = "저장 오류" }, "synthetic storage error response");
        GroupResult(a.Session, X); GroupResult(c.Session, "");
        Check(Status.Contains(storageError, StringComparison.Ordinal) && Status.Contains("저장 오류", StringComparison.Ordinal) && !Status.Contains("이동 대상", StringComparison.Ordinal),
            "storage error survives stale and empty group responses", Status);
        const string collectionError = "수집 실패 · synthetic collection-error response";
        Publish(new UiSnapshot { SessionEpoch = -1, Status = "수집 중단", CommandStatus = collectionError, IsSynthetic = true },
            "synthetic fatal collection frame through controller");
        GroupResult(c.Session, Y);
        Check(Status.Contains("수집 중단", StringComparison.Ordinal) && Status.Contains(collectionError, StringComparison.Ordinal) &&
            !Status.Contains(storageError, StringComparison.Ordinal) && !Status.Contains("이동 대상", StringComparison.Ordinal) &&
            !Status.Contains(Pending, StringComparison.Ordinal) && !Status.Contains(Retention, StringComparison.Ordinal),
            "collection failure displays its own reason and rejects prior-session group response", Status);
        Capture("collection-error-precedence");
    }

    [STAThread] private static int Main(string[] args)
    {
        if (args.Length != 3) return 2;
        output = Path.GetFullPath(args[0]);
        if (Directory.Exists(output)) return 2;
        Directory.CreateDirectory(output);
        string managedPath = typeof(MainWindow).Assembly.Location;
        string nativePath = Path.Combine(AppContext.BaseDirectory, "MonitorBridge.dll");
        string managedHash = Hash(managedPath), nativeHash = Hash(nativePath);
        try {
            Require(managedHash.Equals(args[1], StringComparison.OrdinalIgnoreCase), "loaded managed assembly matches production SHA256");
            Require(nativeHash.Equals(args[2], StringComparison.OrdinalIgnoreCase), "runner native file matches production SHA256");
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            var assembly = typeof(MainWindow).Assembly;
            object options = assembly.GetType("PcMonitor.Options", true)!.GetMethod("Parse")!.Invoke(null, [new[] {
                "--synthetic", "--db", Path.Combine(output, "synthetic.sqlite"), "--interval-ms", "60000",
                "--diagnostics", Path.Combine(output, "worker.jsonl") }])!;
            window = new MainWindow { Width = 1440, Height = 900, ShowActivated = false, ShowInTaskbar = false, Left = -32000, Top = -32000 };
            controllerType = assembly.GetType("PcMonitor.WindowController", true)!;
            controller = Activator.CreateInstance(controllerType, window, options)!;
            loop = controllerType.GetField("loop", Private)!.GetValue(controller)!; loopType = loop.GetType();
            window.Loaded += (_, _) => window.Dispatcher.BeginInvoke(new Action(() => {
                try { Cases(args[2]); }
                catch (Exception e) { ++failures; ledger.Add(new { stage = "unexpected", error = e.ToString(), status = Status }); Console.Error.WriteLine(e); }
                finally { window.Close(); }
            }));
            app.Run(window);
        } catch (Exception e) { ++failures; ledger.Add(new { stage = "setup-failure", error = e.ToString() }); Console.Error.WriteLine(e); }
        File.WriteAllText(Path.Combine(output, "results.json"), JsonSerializer.Serialize(new {
            checks = checks.Count, failures, managedPath, managedSha256 = managedHash, nativePath, nativeSha256 = nativeHash,
            productionManagedAndNative = true, actualWorkerControllerWindow = true, persistedSyntheticNativeInput = true,
            syntheticXAndYResponses = true, nativeLabelsAreFixedSyntheticProcessName = true,
            pendingRetentionAndFatalFramesAreSynthetic = true, actualOomOrStorageFaultInjected = false,
            actualFeedbackAndPauseResume = true, externalLookupOrShellOpen = false, results = checks, ledger }, Json));
        Console.WriteLine($"session group status checks={checks.Count} failures={failures}");
        return failures == 0 ? 0 : 1;
    }
}
