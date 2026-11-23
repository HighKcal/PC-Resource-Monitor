// 정식 DLL의 0/1 ABI와 실제 WPF 선택 경로를 같은 격리 DB 시나리오로 검사한다.
// SQL 실패는 이 시험 DB의 테이블 이름만 잠시 바꾼다. 강제 frame 불일치는 worker
// 예외 경계의 시험이며, 일반 UI가 오래된 sequence를 native에 넘긴다는 증거는 아니다.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal sealed class RecentSql : IDisposable
{
    private IntPtr db;
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_open_v2([MarshalAs(UnmanagedType.LPUTF8Str)] string path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_exec(IntPtr db,[MarshalAs(UnmanagedType.LPUTF8Str)] string sql,IntPtr callback,IntPtr context,IntPtr error);
    [DllImport("winsqlite3.dll")] private static extern IntPtr sqlite3_errmsg(IntPtr db);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_close(IntPtr db);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_prepare_v2(IntPtr db,[MarshalAs(UnmanagedType.LPUTF8Str)]string sql,int bytes,out IntPtr statement,IntPtr tail);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_step(IntPtr statement);
    [DllImport("winsqlite3.dll")] private static extern long sqlite3_column_int64(IntPtr statement,int column);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_finalize(IntPtr statement);
    public RecentSql(string path)
    {
        if(sqlite3_open_v2(path,out db,2,IntPtr.Zero)!=0) throw new IOException("Cannot open recent-query fixture");
        Sql("PRAGMA busy_timeout=1000");
    }
    public void Sql(string sql)
    {
        if(sqlite3_exec(db,sql,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero)!=0)
            throw new IOException(sql+": "+Marshal.PtrToStringUTF8(sqlite3_errmsg(db)));
    }
    public long Scalar(string sql)
    {
        if(sqlite3_prepare_v2(db,sql,-1,out var statement,IntPtr.Zero)!=0) throw new IOException(sql);
        try {if(sqlite3_step(statement)!=100) throw new IOException(sql);return sqlite3_column_int64(statement,0);}
        finally {sqlite3_finalize(statement);}
    }
    public void Dispose() { if(db!=IntPtr.Zero) {sqlite3_close(db);db=IntPtr.Zero;} }
}

internal static class RecentFailures
{
    private const BindingFlags Private=BindingFlags.Instance|BindingFlags.NonPublic;
    private enum RecentResult { Error=0,Success=1,TargetChanged=2 }
    [StructLayout(LayoutKind.Sequential)] private struct RecentMetric
    {
        public uint State,Count;
        public long Anchor;
        public double Current,Median,Duration;
        public ulong Start,End,HistoryStart,HistoryEnd;
    }
    [StructLayout(LayoutKind.Sequential)] private struct RecentComparison
    {
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public RecentMetric[] Metric;
    }
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int bridge_recent(IntPtr b,uint session,ulong sequence,uint pid,ulong creation,long program,[MarshalAs(UnmanagedType.LPUTF8Str)]string path,out RecentComparison result);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern RecentResult bridge_recent_result(IntPtr b,uint session,ulong sequence,uint pid,ulong creation,long program,[MarshalAs(UnmanagedType.LPUTF8Str)]string path,out RecentComparison result);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern IntPtr bridge_error(IntPtr b);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int bridge_failed(IntPtr b);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int bridge_recent_fixture_clock(IntPtr b,ulong utc);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern uint bridge_recent_fixture_queries(IntPtr b);
    private static readonly List<object> checks=[];
    private static int failures;
    private static string output="";
    private static void Check(bool passed,string name,object? detail=null)
    {
        checks.Add(new {name,passed,detail});
        Console.WriteLine((passed?"PASS ":"FAIL ")+name);
        if(!passed) ++failures;
    }
    private static string Error(IntPtr b)=>Marshal.PtrToStringUTF8(bridge_error(b))??"";
    private static IntPtr Handle(Engine engine)=>(IntPtr)typeof(Engine).GetField("handle",Private)!.GetValue(engine)!;
    private static bool Empty(RecentComparison r)=>r.Metric.All(m=>m.State==0 && m.Count==0 && m.Anchor==0 && m.Current==0 && m.Median==0 && m.Duration==0 && m.Start==0 && m.End==0 && m.HistoryStart==0 && m.HistoryEnd==0);
    private static void NativeCases()
    {
        string path=Path.Combine(output,"native.sqlite");
        using var engine=Engine.Open(path,true);
        var s=engine.Collect();var row=s.Rows.Single();var b=Handle(engine);
        int Read(uint session,ulong sequence,ProcessKey key,string requestPath,out RecentComparison r)=>bridge_recent(b,session,sequence,key.Pid,key.Creation,row.ProgramId,requestPath,out r);
        int rc=Read(s.Session,s.Sequence-1,row.Key,row.Path,out var result);
        string first=Error(b);
        Check(rc==0 && first.Length>0 && Empty(result),"legacy stale sequence has current diagnostic and empty output",new {rc,error=first,empty=Empty(result)});
        try {engine.Feedback(s.Session+1,row.Key,row.ProgramId,FeedbackKind.Ignore);} catch(EngineException) { }
        string feedback=Error(b);
        Check(feedback.Length>0,"fixture rejected feedback has an error",feedback);
        rc=Read(s.Session+1,s.Sequence,row.Key,row.Path,out result);
        Check(rc==0 && Error(b).Length>0 && Error(b)!=feedback && Empty(result),"stale session replaces preceding feedback error",new {rc,error=Error(b),feedback});
        rc=Read(s.Session,s.Sequence,row.Key,row.Path,out result);
        Check(rc==1 && result.Metric.All(m=>m.State==0) && Error(b).Length==0,"NO_WINDOW is successful and clears preceding error",new {rc,error=Error(b),states=result.Metric.Select(m=>m.State)});
        rc=Read(s.Session,s.Sequence,row.Key with {Creation=row.Key.Creation+1},row.Path,out result);
        Check(rc==0 && Error(b).Length>0 && Error(b)!=feedback && Empty(result),"changed instance is rejected with its own reason",new {rc,error=Error(b)});
        rc=Read(s.Session,s.Sequence,row.Key,row.Path+".changed",out result);
        Check(rc==0 && Error(b).Length>0 && Empty(result),"changed path is rejected with empty output",new {rc,error=Error(b)});
        bool typed=NativeLibrary.TryGetExport(NativeLibrary.Load(Path.Combine(AppContext.BaseDirectory,"MonitorBridge.dll")),"bridge_recent_result",out _);
        Check(typed,"typed recent-result export is available");
        if(typed) {
            RecentResult status=bridge_recent_result(b,s.Session+1,s.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out result);
            Check(status==RecentResult.TargetChanged && Empty(result),"typed result distinguishes target change",new {status,error=Error(b)});
            status=bridge_recent_result(b,s.Session,s.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,"",out result);
            Check(status==RecentResult.Error && Error(b).Length>0 && Empty(result),"invalid request is an error rather than a target change",new {status,error=Error(b)});
            string ownError=Error(b);RecentResult cross=RecentResult.Success;
            var wrongThread=new Thread(()=>cross=bridge_recent_result(b,s.Session,s.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out _));
            wrongThread.Start();wrongThread.Join();
            Check(cross==RecentResult.Error && Error(b)==ownError,"wrong-thread rejection does not overwrite owner error");
        }
        try {engine.GetRecentComparison(s.Session+1,s.Sequence,row.Key,row.ProgramId,row.Path);Check(false,"legacy managed getter rejects stale target");}
        catch(EngineException e) {Check(e.Message.Length>0 && e.Message!=feedback,"legacy managed getter rejects stale target with current diagnostic",e.Message);}
        var attempt=typeof(Engine).GetMethod("TryGetRecentComparison",Private);
        if(attempt!=null) {
            object[] args=[s.Session+1,s.Sequence,row.Key,row.ProgramId,row.Path,null!];
            Check(!(bool)attempt.Invoke(engine,args)! && ((RecentMetricReport[])args[5]).Length==0,"worker getter returns false only for target change");
        }
        // 새 세션에는 최근 조회 캐시가 없으므로 이름 변경이 실제 SQL 실패를 일으킨다.
        engine.Pause();engine.Resume();s=engine.Collect();row=s.Rows.Single();
        using var sql=new RecentSql(path);
        sql.Sql("ALTER TABLE aggregate_metric RENAME TO fixture_hidden_metric");
        try {
            rc=Read(s.Session,s.Sequence,row.Key,row.Path,out result);
            Check(rc==0 && Error(b).Contains("aggregate_metric",StringComparison.Ordinal) && Empty(result),"real database read failure reports the missing table",new {rc,error=Error(b)});
            Check(bridge_failed(b)==0 && engine.Current!.Sequence==s.Sequence,"recent database read error preserves collection state");
            try {engine.GetRecentComparison(s.Session,s.Sequence,row.Key,row.ProgramId,row.Path);Check(false,"managed database error is thrown");}
            catch(EngineException e) {Check(e.Message.Contains("aggregate_metric",StringComparison.Ordinal),"managed database error is thrown",e.Message);}
            if(typed) Check(bridge_recent_result(b,s.Session,s.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out result)==RecentResult.Error,"typed database result is not target changed");
        } finally {sql.Sql("ALTER TABLE fixture_hidden_metric RENAME TO aggregate_metric");}
        rc=Read(s.Session,s.Sequence,row.Key,row.Path,out result);
        Check(rc==1 && Error(b).Length==0 && result.Metric.All(m=>m.State==0),"recovered query clears database error",new {rc,error=Error(b)});
        ulong sequence=s.Sequence;s=engine.Collect();row=s.Rows.Single();
        Check(s.Sequence>sequence && bridge_failed(b)==0,"collection continues after recent query failure");
        sql.Sql("CREATE TRIGGER fixture_stop_run BEFORE INSERT ON collection_run BEGIN SELECT RAISE(ABORT,'fixture fatal collection'); END");
        try {
            try {engine.Collect();Check(false,"fixture collection failure occurred");} catch(EngineException) { }
            string fatal=Error(b);rc=Read(s.Session,s.Sequence,row.Key,row.Path,out result);
            Check(bridge_failed(b)!=0 && rc==0 && Error(b)==fatal && fatal.Contains("fixture fatal collection",StringComparison.Ordinal),"recent request preserves the fatal collection diagnostic",new {rc,error=Error(b)});
        } finally {sql.Sql("DROP TRIGGER fixture_stop_run");}
    }
    private static void Until(Func<bool> done,string label)
    {
        var clock=Stopwatch.StartNew();
        while(!done()) {
            if(clock.ElapsedMilliseconds>10000) throw new TimeoutException(label);
            var frame=new DispatcherFrame();var timer=new DispatcherTimer {Interval=TimeSpan.FromMilliseconds(20)};
            timer.Tick+=(_,_)=> {timer.Stop();frame.Continue=false;};timer.Start();Dispatcher.PushFrame(frame);
        }
    }
    // 치명적 COMMIT 실패의 원인은 이후 명령의 거절 사유보다 오래 살아야 한다.
    // feedback/edit 자체는 기존 거절 오류를 반환하고, 그 다음 최근 조회가 수집 중단의
    // 실제 원인을 돌려주는지 양쪽 저장 실패와 legacy/typed 조합을 직접 검사한다.
    private static void FatalDiagnosticCases()
    {
        foreach(bool feedbackFailure in new[] {false,true}) {
            string mode=feedbackFailure?"feedback":"collection";
            using var engine=Engine.Open(Path.Combine(output,"fatal-"+mode+".sqlite"),true);
            var snapshot=engine.Collect();var row=snapshot.Rows.Single();IntPtr b=Handle(engine);
            using var sql=new RecentSql(Path.Combine(output,"fatal-"+mode+".sqlite"));
            string table=feedbackFailure?"feedback":"collection_run";
            sql.Sql("CREATE TABLE fixture_recent_bad_fk(p INTEGER REFERENCES program(id) DEFERRABLE INITIALLY DEFERRED);"+
                $"CREATE TRIGGER fixture_recent_commit AFTER INSERT ON {table} BEGIN INSERT INTO fixture_recent_bad_fk VALUES(-1); END");
            string fatal="";
            try {
                try {
                    if(feedbackFailure) engine.Feedback(snapshot.Session,row.Key,row.ProgramId,FeedbackKind.Ignore);
                    else engine.Collect();
                } catch(EngineException e) {fatal=e.Message;}
                Check(bridge_failed(b)!=0 && fatal.Contains("COMMIT",StringComparison.Ordinal) && fatal.Contains("FOREIGN KEY",StringComparison.Ordinal),mode+" fixture failed at COMMIT",fatal);
                foreach(bool rejectedFeedback in new[] {true,false}) foreach(bool typed in new[] {false,true}) {
                    string command=rejectedFeedback?"feedback":"edit";
                    string rejected="";
                    try {
                        if(rejectedFeedback) engine.Feedback(snapshot.Session,row.Key,row.ProgramId,FeedbackKind.Ignore);
                        else engine.Edit(snapshot.Session+1,GroupAction.ProcessTarget,row.Key);
                    } catch(EngineException e) {rejected=e.Message;}
                    string expected=rejectedFeedback?"Feedback target is unavailable or changed; no storage write was made":"Membership target is unavailable or changed; operation was not applied";
                    Check(rejected==expected,mode+" fatal then rejected "+command+" preserves command error ("+(typed?"typed":"legacy")+")",rejected);
                    RecentComparison result;
                    bool failed=typed
                        ? bridge_recent_result(b,snapshot.Session,snapshot.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out result)==RecentResult.Error
                        : bridge_recent(b,snapshot.Session,snapshot.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out result)==0;
                    Check(failed && bridge_failed(b)!=0 && Empty(result) && Error(b)==fatal,
                        mode+" fatal survives rejected "+command+" in "+(typed?"typed":"legacy")+" recent query",
                        new {fatal,rejected,error=Error(b),failed,empty=Empty(result),engineFailed=bridge_failed(b)});
                }
            } finally {sql.Sql("DROP TRIGGER fixture_recent_commit; DROP TABLE fixture_recent_bad_fk");}
            engine.Pause();engine.Resume();
            RecentResult status=bridge_recent_result(b,snapshot.Session,snapshot.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out var pending);
            Check(status==RecentResult.TargetChanged && bridge_failed(b)==0 && Empty(pending) && Error(b).Length>0 && Error(b)!=fatal,mode+" resume discards previous fatal reason before first new collection",Error(b));
            var recovered=engine.Collect();row=recovered.Rows.Single();
            int rc=bridge_recent(b,recovered.Session,recovered.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out var recent);
            Check(rc==1 && bridge_failed(b)==0 && Error(b).Length==0 && recent.Metric.All(m=>m.State==0),mode+" resumed legacy query returns normal NO_WINDOW");
            status=bridge_recent_result(b,recovered.Session,recovered.Sequence,row.Key.Pid,row.Key.Creation,row.ProgramId,row.Path,out recent);
            Check(status==RecentResult.Success && Error(b).Length==0,mode+" resumed typed query returns success");
        }
    }
    private static void Capture(MainWindow window,string name)
    {
        window.UpdateLayout();var image=new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth),(int)Math.Ceiling(window.ActualHeight),96,96,PixelFormats.Pbgra32);
        image.Render(window);var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(image));
        using var stream=File.Create(Path.Combine(output,name+".png"));encoder.Save(stream);
    }
    private static void WpfCases(bool retentionOnly=false)
    {
        var app=new Application {ShutdownMode=ShutdownMode.OnExplicitShutdown};
        var window=new MainWindow {Width=1440,Height=900,ShowActivated=false,ShowInTaskbar=false};
        var assembly=typeof(MainWindow).Assembly;string path=Path.Combine(output,"worker.sqlite");
        object options=assembly.GetType("PcMonitor.Options",true)!.GetMethod("Parse")!.Invoke(null,[new[] {"--synthetic","--db",path,"--interval-ms","100","--diagnostics",Path.Combine(output,"worker.jsonl")}])!;
        var type=assembly.GetType("PcMonitor.WindowController",true)!;object controller=Activator.CreateInstance(type,window,options)!;
        object loop=type.GetField("loop",Private)!.GetValue(controller)!;Type loopType=loop.GetType();
        void Act(string name)=>type.GetMethod("Act",Private)!.Invoke(controller,[new UiCommand(name)]);
        string Text(string name)=>((TextBlock)window.FindName(name)).Text;
        long Requests()=>(long)loopType.GetField("recentRequests",Private)!.GetValue(loop)!;
        // 시험용 명령도 제품 Enqueue를 통해 실제 생성 worker에서 직렬 실행한다.
        void OnWorker(Action action) {
            bool done=false;Exception? error=null;
            loopType.GetMethod("Enqueue",Private)!.Invoke(loop,[new Action(()=> {try {action();} catch(Exception e) {error=e;} finally {Volatile.Write(ref done,true);}})]);
            Until(()=>Volatile.Read(ref done),"worker fixture completed");if(error!=null) throw error;
        }
        void Select(UiSnapshot shown,UiRow? row)=>loopType.GetMethod("SelectRecent")!.Invoke(loop,[shown,row]);
        window.Loaded+=(_,_)=>window.Dispatcher.BeginInvoke(new Action(()=> {
            try {
                Until(()=>window.CurrentSnapshot.Sequence>=2,"first committed rows");Act("pause");Until(()=>window.CurrentSnapshot.Paused,"pause before first recent selection");
                if(retentionOnly) {
                    // engine 생성 worker에서만 합성 6개 완료창을 만든다. UI는 일시정지한 채
                    // 실제 Act→Feedback→trim→RefreshRecent→Publish 동선을 사용한다.
                    OnWorker(()=> {
                        var engine=(Engine)loopType.GetField("engine",Private)!.GetValue(loop)!;
                        engine.Resume();for(int i=0;i<121;++i) engine.Collect();engine.Pause();
                        loopType.GetField("frame",Private)!.SetValue(loop,engine.Current);
                        loopType.GetMethod("ClearRecent",Private)!.Invoke(loop,null);
                        loopType.GetMethod("Publish",Private)!.Invoke(loop,null);
                    });
                    Until(()=>window.CurrentSnapshot.Sequence>=123,"six completed windows published");
                    var initial=window.CurrentSnapshot;var selected=initial.Rows.Single();
                    Check(window.SelectProcess(selected.Key,UiMetric.Cpu),"retention fixture selects current process");
                    Until(()=>window.SelectedRow?.RecentComparisons.Count==4,"initial retention recent result");
                    var ready=window.SelectedRow!.RecentComparisons.Single(r=>r.Metric==UiMetric.Cpu);
                    Check(ready.State==RecentComparisonState.Ready && ready.Count==5 && Text("AverageTitle")=="최근 5개 구간 중앙값",
                        "paused WPF starts with five historical windows and ready card",new {ready.State,ready.Count,ready.Average,ready.End,title=Text("AverageTitle")});
                    using var retentionSql=new RecentSql(path);
                    Check(retentionSql.Scalar("SELECT count(*) FROM aggregate_window")==6,"WPF retention input has six actual windows");
                    ulong latest=(ulong)ready.End!.Value.ToFileTime();
                    ulong trimAt=latest+28UL*86400*10000000-30UL*10000000;
                    void Clock(ulong utc)=>OnWorker(()=> {
                        var engine=(Engine)loopType.GetField("engine",Private)!.GetValue(loop)!;
                        if(bridge_recent_fixture_clock(Handle(engine),utc)==0) throw new IOException("fixture clock rejected");
                    });
                    uint Queries() {uint result=0;OnWorker(()=> {var engine=(Engine)loopType.GetField("engine",Private)!.GetValue(loop)!;result=bridge_recent_fixture_queries(Handle(engine));});return result;}
                    void Feedback(string name) {
                        long before=Requests();var shownBefore=window.CurrentSnapshot;
                        Act(name);OnWorker(()=>{});
                        Until(()=>!ReferenceEquals(shownBefore,window.CurrentSnapshot) && Text("CommandStatusText").Contains("알림 설정을 저장",StringComparison.Ordinal),"feedback published");
                        Check(Requests()==before+1,"successful feedback rechecks only selected recent target",new {before,after=Requests()});
                    }
                    long retainedSequence=initial.Sequence;
                    Clock(latest);Feedback("feedback-suppress-hour");
                    Check(Queries()==0 && window.CurrentSnapshot.Sequence==retainedSequence && window.CurrentSnapshot.Paused,
                        "no-delete paused feedback adds zero recent SQL and preserves collection sequence");
                    Clock(trimAt);
                    retentionSql.Sql("CREATE TRIGGER stop_wpf_recent_trim BEFORE DELETE ON collection_run BEGIN SELECT RAISE(ABORT,'WPF recent trim rollback'); END");
                    try {
                        Feedback("feedback-clear");
                        var rolled=window.SelectedRow!.RecentComparisons.Single(r=>r.Metric==UiMetric.Cpu);
                        Check(retentionSql.Scalar("SELECT count(*) FROM aggregate_window")==6 && rolled.State==RecentComparisonState.Ready && rolled.Count==5 &&
                            Queries()==0 && Text("AverageTitle")=="최근 5개 구간 중앙값","failed retention rolls back history and keeps valid WPF card without SQL");
                    } finally {retentionSql.Sql("DROP TRIGGER stop_wpf_recent_trim");}
                    Clock(trimAt);Feedback("feedback-suppress-hour");
                    var partial=window.SelectedRow!.RecentComparisons.Single(r=>r.Metric==UiMetric.Cpu);
                    Check(retentionSql.Scalar("SELECT count(*) FROM aggregate_window")==1 && partial.State==RecentComparisonState.Preparing && partial.Count==0 &&
                        partial.Average==ready.Average && partial.Start==ready.Start && partial.End==ready.End && Text("AverageValue")=="준비 중" &&
                        Text("RecentComparisonNote").Contains("0/5",StringComparison.Ordinal) && Queries()>0 && window.CurrentSnapshot.Sequence==retainedSequence && window.CurrentSnapshot.Paused,
                        "feedback trim immediately replaces ready card with zero-history preparing card while paused",
                        new {partial.State,partial.Count,partial.Average,partial.Start,partial.End,title=Text("AverageTitle"),value=Text("AverageValue"),note=Text("RecentComparisonNote"),queries=Queries()});
                    Capture(window,"wpf-feedback-history-trim");
                    Clock(latest+29UL*86400*10000000);Feedback("feedback-clear");
                    var empty=window.SelectedRow!.RecentComparisons.Single(r=>r.Metric==UiMetric.Cpu);
                    Check(retentionSql.Scalar("SELECT count(*) FROM aggregate_window")==0 && empty.State==RecentComparisonState.Waiting && empty.Count==0 &&
                        empty.Average==null && empty.Start==null && empty.End==null && Text("UpperValue")=="완료 구간 대기" &&
                        window.CurrentSnapshot.Sequence==retainedSequence && window.CurrentSnapshot.Paused,
                        "feedback trim immediately clears deleted latest-window values and time on paused WPF card",
                        new {empty.State,empty.Count,empty.Average,empty.Start,empty.End,value=Text("UpperValue"),time=Text("UpperTime")});
                    Capture(window,"wpf-feedback-latest-trim");
                    return;
                }
                var s=window.CurrentSnapshot;var row=s.Rows.Single();
                Check(Requests()==0,"normal unselected WPF list has no recent query");
                using var sql=new RecentSql(path);
                sql.Sql("ALTER TABLE aggregate_metric RENAME TO fixture_hidden_metric");
                try {
                    Check(window.SelectProcess(row.Key,UiMetric.Cpu),"ordinary WPF process selection accepted");
                    Until(()=>Text("CommandStatusText").Contains("aggregate_metric",StringComparison.Ordinal),"native database failure reached WPF status");
                    Check(Text("CommandStatusText").Contains("최근 기록 비교를 확인할 수 없습니다",StringComparison.Ordinal) && !Text("CollectionStatus").Contains("중단",StringComparison.Ordinal) && window.SelectedRow!.RecentComparisons.Count==0,
                        "ordinary selection displays actual SQL error without stopping collection",new {command=Text("CommandStatusText"),status=Text("CollectionStatus")});
                    Capture(window,"wpf-database-error");
                } finally {sql.Sql("ALTER TABLE fixture_hidden_metric RENAME TO aggregate_metric");}
                window.SelectGroup(s.Groups.Single().Key);OnWorker(()=>{});
                Check(window.SelectProcess(row.Key,UiMetric.Cpu),"reselect after database recovery");
                Until(()=>window.SelectedRow?.RecentComparisons.Count==4,"recovered recent selection");
                Check(window.SelectedRow!.RecentComparisons.All(r=>r.State==RecentComparisonState.Waiting),"ordinary successful NO_WINDOW stays a result rather than an error");
                long requests=Requests();string status=Text("CommandStatusText");
                Select(s with {SessionEpoch=s.SessionEpoch+1},row);Select(s,row with {Path=row.Path+".changed"});OnWorker(()=>{});
                Check(Requests()==requests && Text("CommandStatusText")==status,"stale selection payload is discarded before native and produces no new warning");
                Select(s with {Sequence=0},row);OnWorker(()=>{});
                Check(Requests()==requests+1,"old UI sequence is refreshed using worker current sequence");
                // native와 managed frame의 불일치를 의도적으로 만든다. 일반 동선에서
                // 같은 worker의 Collect가 frame도 함께 교체한다는 계약과 구분하여 기록한다.
                window.SetCommandStatus("fixture status before target change");status=Text("CommandStatusText");
                OnWorker(()=> {
                    var engine=(Engine)loopType.GetField("engine",Private)!.GetValue(loop)!;
                    engine.Resume();engine.Collect();
                    loopType.GetField("recentSequence",Private)!.SetValue(loop,ulong.MaxValue);
                    loopType.GetMethod("RefreshRecent",Private)!.Invoke(loop,null);
                    engine.Pause();loopType.GetField("frame",Private)!.SetValue(loop,engine.Current);
                    loopType.GetMethod("ClearRecent",Private)!.Invoke(loop,null);
                    loopType.GetMethod("Publish",Private)!.Invoke(loop,null);
                });
                Until(()=>window.CurrentSnapshot.SessionEpoch!=s.SessionEpoch,"fixture frame resynchronized");
                Check(Text("CommandStatusText")==status,"forced native target change is silent at worker boundary",new {before=status,after=Text("CommandStatusText"),source="injected frame mismatch, not ordinary UI"});
                long sequence=window.CurrentSnapshot.Sequence;Act("resume");
                Until(()=>!window.CurrentSnapshot.Paused && window.CurrentSnapshot.Sequence>sequence,"ordinary resume keeps collecting");
                Check(!Text("CollectionStatus").Contains("중단",StringComparison.Ordinal),"WPF collection advances after database and target-change cases",new {sequence=window.CurrentSnapshot.Sequence,status=Text("CollectionStatus")});
                window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key,UiMetric.Cpu);
                Until(()=>window.SelectedRow?.RecentComparisons.Count==4,"new-session selected result");
                Capture(window,"wpf-recovered");
            } catch(Exception e) {Check(false,"unexpected WPF fixture exception",e.ToString());}
            finally {window.Close();}
        }));
        app.Run(window);
    }
    [STAThread] private static int Main(string[] args)
    {
        if(args.Length!=2 && (args.Length!=3 || args[2]!="--retention-native")) return 2;
        output=Path.GetFullPath(args[0]);if(Directory.Exists(output)) return 2;Directory.CreateDirectory(output);
        string sha=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(typeof(Engine).Assembly.Location)));
        Check(sha.Equals(args[1],StringComparison.OrdinalIgnoreCase),"product assembly SHA matches",sha);
        bool retentionOnly=args.Length==3;
        try {if(!retentionOnly) {NativeCases();FatalDiagnosticCases();} WpfCases(retentionOnly);} catch(Exception e) {Check(false,"unexpected fixture exception",e.ToString());}
        File.WriteAllText(Path.Combine(output,"results.json"),JsonSerializer.Serialize(new {failures,count=checks.Count,sha,actualWpf=true,syntheticDatabase=true,forcedTargetChangeIsOrdinaryUi=false,checks},new JsonSerializerOptions {WriteIndented=true}));
        Console.WriteLine($"recent failure checks={checks.Count} failures={failures}");return failures==0?0:1;
    }
}
