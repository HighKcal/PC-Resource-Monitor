// 새 임시 DB와 실제 WPF worker로 저장 경고를 확인한다. 기존 DB 경로는 거부한다.
// retry는 COMMIT 후 보관 정리 실패→재시도, ingest/feedback은 COMMIT 실패를 주입한다.
// 별도 연결로 남은 행을 관측하고, 창 닫기 경로가 worker/engine 정리를 마치게 한다.
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using PcMonitor;

// 두 번째 연결은 커밋된 행만 관측하고, 이 시험 DB에 실패 주입 trigger를 설치한다.
internal sealed class Observer : IDisposable
{
    private const string Dll = "winsqlite3.dll";
    private IntPtr db;
    private delegate int Callback(IntPtr context, int columns, IntPtr values, IntPtr names);
    [DllImport(Dll)] private static extern int sqlite3_open_v2([MarshalAs(UnmanagedType.LPUTF8Str)] string path, out IntPtr db, int flags, IntPtr vfs);
    [DllImport(Dll)] private static extern int sqlite3_exec(IntPtr db, [MarshalAs(UnmanagedType.LPUTF8Str)] string sql, Callback? callback, IntPtr ctx, IntPtr error);
    [DllImport(Dll)] private static extern IntPtr sqlite3_errmsg(IntPtr db);
    [DllImport(Dll)] private static extern int sqlite3_close(IntPtr db);

    public Observer(string path)
    {
        if (sqlite3_open_v2(path, out db, 2, IntPtr.Zero) != 0) throw new IOException("Cannot open fixture");
        Sql("PRAGMA busy_timeout=1000");
    }
    public string? Sql(string sql)
    {
        string? result = null;
        int rc = sqlite3_exec(db, sql, (_, _, values, _) => {
            result = Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(values));
            return 0;
        }, IntPtr.Zero, IntPtr.Zero);
        if (rc != 0) throw new IOException($"{sql}: {Marshal.PtrToStringUTF8(sqlite3_errmsg(db))}");
        return result;
    }
    public int Count(string condition) => int.Parse(Sql("SELECT count(*) FROM " + condition)!);
    public void Dispose() => sqlite3_close(db);
}

internal static class StorageFailures
{
    private const string Warning = "오래된 기록 정리 지연";
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        Console.WriteLine("PASS " + message);
    }
    private static void Until(Func<bool> condition, string message)
    {
        var clock = Stopwatch.StartNew();
        while (!condition()) {
            if (clock.ElapsedMilliseconds >= 8000) throw new TimeoutException(message);
            var frame = new DispatcherFrame();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
            timer.Tick += (_, _) => { timer.Stop(); frame.Continue = false; };
            timer.Start();
            Dispatcher.PushFrame(frame);
        }
    }
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length != 2 || args[0] is not ("retry" or "ingest" or "feedback")) return 2;
        string path = Path.GetFullPath(args[1]);
        if (File.Exists(path) || Directory.GetFiles(Path.GetDirectoryName(path)!, Path.GetFileName(path) + "-*").Length != 0) return 2;
        using (var seed = Engine.Open(path, true)) seed.Collect();
        using var observer = new Observer(path);
        observer.Sql("BEGIN; UPDATE raw SET utc=1; INSERT INTO feedback(program_id,kind,created,expires) SELECT id,1,0,1 FROM program;" +
                     "CREATE TRIGGER stop_trim BEFORE DELETE ON feedback BEGIN SELECT RAISE(ABORT,'stop cleanup after raw'); END; COMMIT");
        var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
        var window = new MainWindow { ShowActivated=false, ShowInTaskbar=false, Left=-32000, Top=-32000 };
        var assembly = typeof(MainWindow).Assembly;
        var options = assembly.GetType("PcMonitor.Options")!.GetMethod("Parse")!.Invoke(null,
            new object[] { new[] { "--synthetic", "--db", path, "--interval-ms", "350" } });
        var type = assembly.GetType("PcMonitor.WindowController")!;
        var controller = Activator.CreateInstance(type, window, options)!;
        void Act(string name) => type.GetMethod("Act", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(controller, new object[] { new UiCommand(name) });
        string Text(string name) => ((TextBlock)window.FindName(name)).Text;
        string Status() => Text("CollectionStatus");
        string Command() => Text("CommandStatusText");
        int exit = 1;
        window.Loaded += (_, _) => window.Dispatcher.BeginInvoke(new Action(() => {
            try {
                Until(() => window.CurrentSnapshot.Sequence >= 2 && Status().Contains(Warning), "two committed samples with delayed cleanup");
                Check(observer.Count("raw WHERE utc=1") == 1 && observer.Count("feedback WHERE expires=1") == 1, "earlier DELETE rolled back after repeated middle failure");
                Check(observer.Count("collection_run") >= 3, "new samples committed during delay");
                Console.WriteLine("Collection status: " + Status());
                Check(window.SelectProcess(window.CurrentSnapshot.Rows[0].Key), "selected synthetic process");
                Act("feedback-suppress-hour");
                Until(() => Command().Contains("저장했습니다") && window.SelectedRow?.Feedback.Contains("억제") == true, "committed suppression displayed");
                Check(Status().Contains(Warning) && observer.Count("feedback WHERE kind=1 AND cancelled=0 AND expires>1") == 1, "suppression committed while warning stays visible");
                if (args[0] == "retry") {
                    Act("pause");
                    Until(() => window.CurrentSnapshot.Paused, "worker paused");
                    Check(Status().Contains(Warning), "pause preserves outstanding warning");
                    Act("feedback-clear");
                    Until(() => observer.Count("feedback WHERE kind=0") == 1 && window.SelectedRow?.Feedback == "", "committed cancel displayed");
                    Check(Status().Contains(Warning), "cancel preserves outstanding warning");
                    observer.Sql("DROP TRIGGER stop_trim");
                    Act("resume");
                    Until(() => !window.CurrentSnapshot.Paused && !Status().Contains(Warning) && observer.Count("raw WHERE utc=1") == 0, "next collection cleared warning and expired raw");
                    Check(observer.Count("feedback WHERE expires=1") == 0 && observer.Count("feedback WHERE kind=0") == 1, "expired feedback removed and committed cancel retained");
                    Console.WriteLine("Recovered status: " + Status());
                } else {
                    // 지연 FK로 개별 SQL은 성공시키고 실제 COMMIT에서 실패하게 만든다.
                    Act("pause");
                    Until(() => window.CurrentSnapshot.Paused, "worker paused for fault installation");
                    var table = args[0] == "ingest" ? "collection_run" : "feedback";
                    observer.Sql($"CREATE TABLE bad_fk(p INTEGER REFERENCES program(id) DEFERRABLE INITIALLY DEFERRED);" +
                                 $"CREATE TRIGGER fail_commit AFTER INSERT ON {table} BEGIN INSERT INTO bad_fk VALUES(-1); END");
                    int runs = observer.Count("collection_run");
                    if (args[0] == "ingest") Act("resume"); else Act("feedback-clear");
                    Until(() => Status().Contains("중단") && Command().Contains("FOREIGN KEY"), "fatal commit failure visible");
                    Console.WriteLine("Failed status: " + Command());
                    Check(!Status().Contains(Warning) && !Command().Contains("저장했습니다"), "failure takes precedence over cleanup warning and old success");
                    Check(observer.Count("collection_run") == runs && observer.Count("bad_fk") == 0, "failed commit left no run or fault rows");
                    Check(observer.Count("feedback WHERE kind=0") == 0 && observer.Count("feedback WHERE kind=1 AND cancelled=0 AND expires>1") == 1, "original suppression survived failed write");
                }
                Check(observer.Sql("PRAGMA integrity_check") == "ok" && observer.Sql("PRAGMA foreign_key_check") == null, "database integrity ok");
                exit = 0;
            } catch (Exception ex) { Console.Error.WriteLine(ex); Console.Error.WriteLine(Command()); }
            finally { window.Close(); }
        }));
        app.Run(window);
        return exit;
    }
}
