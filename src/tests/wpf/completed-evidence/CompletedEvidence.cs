// 합성 입력 → 실제 C 엔진/SQLite → WPF worker/WindowController → 세 카드의 회귀다.
// CPU 결측은 시험 native의 monitor_ingest 직전 한 표본에만 주입한다. 아래 DisplayCases는
// 별도로 표시한 불변 입력 대조이며 일반 동선의 관측과 혼동하지 않는다. 외부 Python은 쓰지 않는다.
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
using System.Text.Json.Serialization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal sealed class CompletedSql : IDisposable
{
    private IntPtr db;
    private delegate int Callback(IntPtr context,int count,IntPtr values,IntPtr names);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_open_v2([MarshalAs(UnmanagedType.LPUTF8Str)] string path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_exec(IntPtr db,[MarshalAs(UnmanagedType.LPUTF8Str)] string sql,Callback? callback,IntPtr context,IntPtr error);
    [DllImport("winsqlite3.dll")] private static extern IntPtr sqlite3_errmsg(IntPtr db);
    [DllImport("winsqlite3.dll")] private static extern int sqlite3_close(IntPtr db);
    public CompletedSql(string path)
    {
        if(sqlite3_open_v2(path,out db,2,IntPtr.Zero)!=0) throw new IOException("Cannot open isolated completed-window fixture");
        Query("PRAGMA busy_timeout=1000; PRAGMA foreign_keys=ON");
    }
    public List<Dictionary<string,string?>> Query(string sql)
    {
        List<Dictionary<string,string?>> rows=[];
        int result=sqlite3_exec(db,sql,(_,count,values,names)=> {
            Dictionary<string,string?> row=[];
            for(int i=0;i<count;++i) row.Add(Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(names,i*IntPtr.Size))!,
                Marshal.PtrToStringUTF8(Marshal.ReadIntPtr(values,i*IntPtr.Size)));
            rows.Add(row);return 0;
        },IntPtr.Zero,IntPtr.Zero);
        if(result!=0) throw new IOException(sql+": "+Marshal.PtrToStringUTF8(sqlite3_errmsg(db)));
        return rows;
    }
    public long Scalar(string sql)=>long.Parse(Query(sql).Single().Values.Single()!,CultureInfo.InvariantCulture);
    public void Dispose() {if(db!=IntPtr.Zero) {sqlite3_close(db);db=IntPtr.Zero;}}
}

internal static class CompletedEvidence
{
    private const BindingFlags Private=BindingFlags.Instance|BindingFlags.NonPublic;
    private static readonly JsonSerializerOptions Json=new() {WriteIndented=true,NumberHandling=JsonNumberHandling.AllowNamedFloatingPointLiterals};
    private static readonly List<object> Checks=[];
    private sealed record Observation(Snapshot Native,UiSnapshot Ui,Dictionary<string,string> Controls,Dictionary<string,string?> Raw,Dictionary<string,string?> Window);
    private static readonly Dictionary<long,Observation> Frames=[];
    private static readonly string[] CardNames=["AnalysisTarget","AnalysisReason","AverageTitle","AverageValue","AverageTime","UpperTitle","UpperValue","UpperTime","CurrentTitle","CurrentValue","CurrentTime","RecentComparisonNote","CommandStatusText"];
    private static int failures,observedControllerFrames;
    private static string output="",db="";
    private static MainWindow window=null!;
    private static bool selected,seeded;
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void completed_fixture_mode(int mode);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int completed_fixture_policy(IntPtr bridge,int begin);
    private static string Text(string name)=>((TextBlock)window.FindName(name)).Text;
    private static void Check(bool ok,string name,object? detail=null)
    {Checks.Add(new {name,passed=ok,detail});Console.WriteLine((ok?"PASS ":"FAIL ")+name);if(!ok)++failures;}
    private static void Save(string name,object value)=>File.WriteAllText(Path.Combine(output,name),JsonSerializer.Serialize(value,Json));
    private static double Number(Dictionary<string,string?> row,string name)=>double.Parse(row[name]!,CultureInfo.InvariantCulture);
    private static ulong Ticks(Dictionary<string,string?> row,string name)=>ulong.Parse(row[name]!,CultureInfo.InvariantCulture);
    private static MetricState? Completed(MetricReport report)=>(MetricState?)typeof(MetricReport).GetProperty("CompletedState")?.GetValue(report);
    private static object? CompletedField(MetricReport report,string field)
    {
        var evidence=typeof(MetricReport).GetProperty("Completed")?.GetValue(report);
        return evidence?.GetType().GetProperty(field)?.GetValue(evidence);
    }
    private static DateTimeOffset Time(ulong value)=>DateTimeOffset.FromFileTime((long)value).ToUniversalTime();
    private static string Clock(ulong value)=>Time(value).ToLocalTime().ToString("HH:mm:ss",CultureInfo.InvariantCulture);

    // 학습 자료는 같은 program의 과거 세션·과거 인스턴스다. 현재 인스턴스의 creation보다
    // 이른 완료창을 현재 인스턴스로 위장하지 않는다. 각 run은 해당 창 종료 시각에 속한다.
    private static void Seed(string path,double value,string label)
    {
        using var sql=new CompletedSql(path);
        var latest=sql.Query("SELECT program_id,end_utc FROM aggregate_window ORDER BY id DESC LIMIT 1").Single();
        long program=long.Parse(latest["program_id"]!,CultureInfo.InvariantCulture);
        long end=long.Parse(latest["end_utc"]!,CultureInfo.InvariantCulture);
        const long second=10000000,day=86400*second;
        long creation=end-4*day;
        sql.Query($"BEGIN IMMEDIATE; INSERT INTO session(id,started,ended) VALUES(-1,{creation},{end-day+second});");
        for(int j=0;j<60;++j) {
            long finish=end-(1+j/20)*day-(j%20)*60*second;
            long run=-1001-j;
            string mean=value.ToString("R",CultureInfo.InvariantCulture);
            sql.Query($"INSERT INTO collection_run(id,session_id,started,finished,status,row_count,cpu_count) VALUES({run},-1,{finish-60*second},{finish},0,0,0);"+
                $"INSERT INTO aggregate_window(run_id,program_id,pid,creation,start_utc,end_utc,duration,version) VALUES({run},{program},4241,{creation},{finish-60*second},{finish},60,1);"+
                $"INSERT INTO aggregate_metric VALUES(last_insert_rowid(),0,{mean},60,20,1,1,'isolated prior-instance CPU history',0,0,0,NULL,NULL,NULL,0);");
        }
        sql.Query("COMMIT");
        Check(sql.Scalar("SELECT count(*) FROM aggregate_window WHERE run_id<0")==60 &&
            sql.Scalar("SELECT count(*) FROM aggregate_window w JOIN collection_run r ON r.id=w.run_id WHERE w.run_id<0 AND (w.creation>w.start_utc OR w.end_utc>r.finished)")==0,
            label+": 60 eligible past windows have a prior instance and consistent run times");
        Save(label+"-seed.json",new {program,value,creation,windows=60,days=3,priorSession=-1,priorPid=4241,
            currentSessionPastWindows=sql.Scalar("SELECT count(*) FROM aggregate_window w JOIN collection_run r ON r.id=w.run_id WHERE r.session_id>0")});
    }
    private static void Capture(string name,Snapshot native,UiSnapshot ui)
    {
        window.UpdateLayout();
        using var sql=new CompletedSql(db);
        var raw=sql.Query("SELECT id,run_id,utc,pid,creation,program_id,valid,cpu,cpu_interval FROM raw ORDER BY id DESC LIMIT 1").Single();
        var saved=sql.Query("SELECT w.id,w.run_id,w.program_id,w.pid,w.creation,w.start_utc,w.end_utc,m.value,m.threshold,m.state,m.covered,m.valid_count,m.streak FROM aggregate_window w JOIN aggregate_metric m ON m.window_id=w.id WHERE m.metric=0 AND w.run_id>0 ORDER BY w.end_utc DESC,w.id DESC LIMIT 1").Single();
        var controls=CardNames.ToDictionary(n=>n,Text);
        Frames.Add(ui.Sequence,new(native,ui,controls,raw,saved));
        Save(name+"-frame.json",new {native,ui,controls,raw,window=saved,selectedKey=window.SelectedProcessKey,metric=window.SelectedMetric.ToString(),ordinaryControllerSelection=true});
        Screenshot(name);
    }
    private static void Screenshot(string name)
    {
        window.UpdateLayout();
        var bitmap=new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth),(int)Math.Ceiling(window.ActualHeight),96,96,PixelFormats.Pbgra32);
        bitmap.Render(window);var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream=File.Create(Path.Combine(output,name+".png"));png.Save(stream);
    }
    private static void VerifyFlow()
    {
        Check(seeded,"seed inserted after six product-completed windows");
        Check(Frames.Keys.Order().SequenceEqual(new long[] {141,142,143,144,161}),"observed normal, one missing CPU, raw recovery and new completed window",Frames.Keys.Order().ToArray());
        if(Frames.Count!=5)return;
        var normal=Frames[142];var missing=Frames[143];var recovered=Frames[144];var next=Frames[161];
        var n=normal.Native.Rows.Single();var m=missing.Native.Rows.Single();var r=recovered.Native.Rows.Single();var last=next.Native.Rows.Single();
        Check(n.Valid.HasFlag(ResourceValid.Cpu) && !m.Valid.HasFlag(ResourceValid.Cpu) && r.Valid.HasFlag(ResourceValid.Cpu),"CPU is missing for exactly one observed sample");
        Check((n.Valid^m.Valid)==ResourceValid.Cpu && n.Key==m.Key && n.Key==r.Key && n.ProgramId==m.ProgramId && n.Path==m.Path,"only CPU validity changes; PID, creation, path and program remain identical");
        Check(n.Metric[0].State==MetricState.Normal && n.Metric[0].Count>=60 && n.Metric[0].Days>=3 && n.Metric[0].Current==50 && n.Metric[0].Threshold==75,"engine calculated a mature 50/75 CPU window",n.Metric[0]);
        Check(Frames.Values.All(f=>f.Ui.Rows.Single().Alerts.Count==0 && f.Ui.Rows.Single().Alert==null),"ordinary detail has no accepted or clicked alert evidence");
        foreach(var entry in Frames) {
            var frame=entry.Value;var row=frame.Native.Rows.Single();var report=row.Metric[0];var control=frame.Controls;
            Check(report.Current==Number(frame.Window,"value") && report.Threshold==Number(frame.Window,"threshold") &&
                report.Start==Ticks(frame.Window,"start_utc") && report.End==Ticks(frame.Window,"end_utc") && row.Key.Creation==Ticks(frame.Window,"creation") &&
                row.Key.Pid==Number(frame.Window,"pid") && row.ProgramId==Number(frame.Window,"program_id"),
                $"frame {entry.Key}: average, upper and both times belong to one saved window for this instance");
            Check(control["AverageValue"]=="50%" && control["UpperValue"]=="75%" &&
                control["CurrentValue"]==(entry.Key==143?"측정 불가":"50%"),$"frame {entry.Key}: current / average / upper retain their independent values",control);
            Check(control["AverageTime"].Contains(Clock(report.Start)) && control["AverageTime"].Contains(Clock(report.End)) &&
                control["UpperTime"].Contains(Clock(report.End)) && control["CurrentTime"].Contains(Clock(row.MeasuredUtc)),
                $"frame {entry.Key}: cards use saved window times and current observation time");
        }
        Check(normal.Window.SequenceEqual(missing.Window) && normal.Window.SequenceEqual(recovered.Window),"same stored completed row remains unchanged during missing and current recovery");
        Check(missing.Raw["cpu"]==null && missing.Raw["cpu_interval"]==null && Ticks(missing.Raw,"utc")==m.MeasuredUtc,"missing current CPU and its interval are SQL NULL with this observation time");
        Check(missing.Controls["CurrentTime"].EndsWith("관측 · 값 없음") && missing.Controls["CurrentTime"]!=normal.Controls["CurrentTime"] &&
            missing.Controls["AverageTime"]==normal.Controls["AverageTime"] && missing.Controls["UpperTime"]==normal.Controls["UpperTime"],"missing current timestamp advances while completed times stay fixed");
        Check(m.Metric[0].State==MetricState.Unavailable && m.Metric[0].Streak==0 && Completed(m.Metric[0])==MetricState.Normal &&
            recovered.Native.Rows.Single().Metric[0].State==MetricState.Unavailable && Completed(r.Metric[0])==MetricState.Normal,
            "current missing resets the policy state but preserves completed Normal through raw recovery",new {missing=m.Metric[0],recovered=r.Metric[0]});
        Check(missing.Ui.Rows.Single().Comparisons[0].Ready && !missing.Ui.Rows.Single().Comparisons[0].Candidate &&
            missing.Ui.Rows.Single().RecentComparisons[0].State==RecentComparisonState.LongTerm,
            "completed readiness wins over recent fallback without reviving a current candidate");
        Check(next.Native.CompletedWindows==1 && last.Metric[0].CoveredSeconds==57 && last.Metric[0].ValidSamples==19 &&
            next.Window["id"]!=missing.Window["id"] && next.Controls["AverageTime"]!=missing.Controls["AverageTime"] && next.Controls["UpperTime"]!=missing.Controls["UpperTime"],
            "new completed window replaces both saved times together with 19 samples and 57 seconds",last.Metric[0]);
        using var sql=new CompletedSql(db);
        Check(sql.Scalar("SELECT count(*) FROM raw")==161 && sql.Scalar("SELECT count(*) FROM raw WHERE (valid&4)=0")==1 &&
            sql.Scalar("SELECT run_id FROM raw WHERE (valid&4)=0")==143,"committed raw ledger contains exactly one CPU-missing sample among 161");
        Check(sql.Query("PRAGMA integrity_check").Single().Values.Single()=="ok" && sql.Query("PRAGMA foreign_key_check").Count==0,"isolated database integrity and foreign keys");
        Check(sql.Scalar("SELECT count(*) FROM session")==2 && sql.Scalar("SELECT count(*) FROM session WHERE ended IS NULL")==0,"past fixture and actual product sessions ended");
    }

    // 이하 대조는 native 엔진에 순차 입력한 결과다. UI를 대신한 가짜 상태가 아니다.
    private static void NativeControls()
    {
        completed_fixture_mode(1);
        using(var engine=Engine.Open(Path.Combine(output,"candidate.sqlite"),true)) {
            Snapshot frame=null!;MetricReport? candidate=null;int request=0;
            var handle=(IntPtr)typeof(Engine).GetField("handle",Private)!.GetValue(engine)!;
            for(int i=1;i<=183;++i) {
                frame=engine.Collect();if(i==121)Seed(Path.Combine(output,"candidate.sqlite"),10,"candidate");
                int policyRequest=completed_fixture_policy(handle,i is 181 or 182?-1:0);
                if(i==181) {candidate=frame.Rows.Single().Metric[0];request=policyRequest;}
                if(i==182) {
                    var report=frame.Rows.Single().Metric[0];
                    Check(candidate?.State==MetricState.Candidate && candidate.Streak>=2 && request==1,"test policy over actual product frames queues a pending event for the current candidate before missing",candidate);
                    Check(report.State==MetricState.Unavailable && report.Streak==0 && Completed(report)==MetricState.Candidate && policyRequest==0 && engine.BeginNotification()==null,
                        "missing breaks current candidate streak and cancels pending policy evidence while completed Candidate is retained; synthetic product still suppresses Shell",report);
                }
            }
            Check(frame.Rows.Single().Valid.HasFlag(ResourceValid.Cpu) && frame.Rows.Single().Metric[0].State==MetricState.Unavailable && frame.Rows.Single().Metric[0].Streak==0,
                "raw recovery does not restart a candidate before a new window");
            engine.Pause();engine.Resume();frame=engine.Collect();
            Check(frame.Rows.All(r=>r.Metric.All(m=>Completed(m)==null && m.Start==0 && m.End==0)),"pause/resume reset cannot reuse a prior completed window");
        }
        completed_fixture_mode(3);
        using(var engine=Engine.Open(Path.Combine(output,"instance.sqlite"),true)) {
            Snapshot previous=null!,changed=null!;engine.SetNotifications(false);
            for(int i=1;i<=144;++i) {var f=engine.Collect();if(i==121)Seed(Path.Combine(output,"instance.sqlite"),50,"instance");if(i==143)previous=f;if(i==144)changed=f;}
            var a=previous.Rows.Single();var b=changed.Rows.Single();
            Check(a.Key.Pid==b.Key.Pid && a.Key.Creation!=b.Key.Creation && a.Metric[0].End>0 && b.Metric.All(m=>Completed(m)==null && m.Start==0 && m.End==0),
                "same PID with new creation has no prior instance completed values",new {before=a.Key,after=b.Key,metric=b.Metric[0]});
        }
        completed_fixture_mode(2);
        using(var engine=Engine.Open(Path.Combine(output,"zero.sqlite"),true)) {
            Snapshot f=null!;engine.SetNotifications(false);
            for(int i=1;i<=141;++i) {f=engine.Collect();if(i==121)Seed(Path.Combine(output,"zero.sqlite"),0,"zero");}
            var r=f.Rows.Single();Check(r.Valid.HasFlag(ResourceValid.Cpu) && r.Cpu==0 && r.Metric[0].Current==0 && Completed(r.Metric[0])==MetricState.Normal,
                "valid zero survives actual raw collection, aggregation and completed metadata",r.Metric[0]);
        }
        completed_fixture_mode(4);
        using(var engine=Engine.Open(Path.Combine(output,"new-session.sqlite"),true)) {
            engine.Collect();var f=engine.Collect();var r=f.Rows.Single();
            Check(!r.Valid.HasFlag(ResourceValid.Cpu) && Completed(r.Metric[0])==null && r.Metric[0].End==0,"a new session missing before any complete window has no completed evidence");
        }
        foreach(int mode in new[] {5,6,7}) {
            string name=mode==5?"long-gap":mode==6?"utc-reverse":"unavailable-window";
            completed_fixture_mode(mode);
            using var engine=Engine.Open(Path.Combine(output,name+".sqlite"),true);
            engine.SetNotifications(false);MetricReport? prior=null;ProcessRow? previous=null;Snapshot frame=null!;
            for(int i=1;i<=(mode==7?161:144);++i) {
                frame=engine.Collect();if(i==121)Seed(Path.Combine(output,name+".sqlite"),50,name);
                var row=frame.Rows.Single();var report=row.Metric[0];
                if(i==141)prior=report;
                if(mode==5 && i is 143 or 144) {
                    var comparison=ProjectNative(report);
                    double elapsed=previous==null?0:(row.MeasuredUtc-previous.MeasuredUtc)/10000000d;
                    Check(previous!=null && previous.Key==row.Key && previous.Path==row.Path && previous.ProgramId==row.ProgramId &&
                        elapsed==(i==143?16:3) && row.ObservationInterval==elapsed && row.IoInterval==elapsed &&
                        row.Valid.HasFlag(ResourceValid.Cpu)==(i==144) && (i!=144 || row.Cpu==50),
                        $"long gap frame {i}: same instance has a real 16-second forward gap or 3-second raw recovery",new {elapsed,row});
                    // 15초를 넘는 관측은 step.reset을 지나 live 보고서 전체를 비운다.
                    // 단순 CPU 결측(Unavailable, 이전 Current/Start/End 유지)과 반드시 구별한다.
                    Check(report.State==MetricState.Learning && report.Start==0 && report.End==0 && report.Streak==0 &&
                        double.IsNaN(report.Current) && double.IsNaN(report.Threshold) && frame.CompletedWindows==0,
                        $"long gap frame {i}: live window actually reset and has no newly completed values",report);
                    Check(prior!=null && Completed(report)==MetricState.Normal &&
                        (double?)CompletedField(report,"Average")==50 && (double?)CompletedField(report,"Upper")==75 &&
                        (ulong?)CompletedField(report,"Start")==prior.Start && (ulong?)CompletedField(report,"End")==prior.End &&
                        comparison.Average==50 && comparison.Upper==75 && comparison.Start==Time(prior.Start) && comparison.End==Time(prior.End),
                        $"long gap frame {i}: completed 50/75 and its original times survive reset and raw recovery",new {report,comparison});
                }
                if(mode==6 && i is 143 or 144)
                    Check(Completed(report)==null && ProjectNative(report).Upper==null,$"UTC reversal frame {i}: future completed evidence is not carried forward",report);
                previous=row;
            }
            if(mode==7) {
                var report=frame.Rows.Single().Metric[0];var comparison=ProjectNative(report);
                Check(frame.CompletedWindows==1 && Completed(report)==MetricState.Unavailable && prior!=null &&
                    (ulong?)CompletedField(report,"End")>prior.End && comparison.Upper==null,
                    "new unavailable completed window replaces prior mature upper instead of borrowing it",new {report,comparison});
            }
        }
        completed_fixture_mode(0);
    }
    private static UiComparison ProjectNative(MetricReport report)=>(UiComparison)typeof(MainWindow).Assembly.GetType("PcMonitor.Projection",true)!
        .GetMethod("Compare",BindingFlags.Static|BindingFlags.NonPublic)!.Invoke(null,[report,0])!;
    private static UiComparison Project(MetricReport report,MetricState? completed)
    {
        // reflection은 수정 전 assembly에도 같은 flow runner를 실행하기 위한 호환 경계다.
        if(typeof(MetricReport).GetProperty("Completed") is { } property) {
            var type=Nullable.GetUnderlyingType(property.PropertyType)!;
            object? evidence=completed is { } state?Activator.CreateInstance(type,state,report.Current,report.Median,report.Threshold,report.Start,report.End,report.Duration):null;
            property.SetValue(report,evidence);
        } else if(typeof(MetricReport).GetProperty("CompletedState") is {CanWrite:true} stateProperty)stateProperty.SetValue(report,completed);
        return ProjectNative(report);
    }
    private static void DisplayCases(UiSnapshot basis,MetricReport normal)
    {
        var row=basis.Rows.Single();
        void Show(MetricReport metric,MetricState? completed,double? current,UiAlert? alert=null)
        {
            var comparison=Project(metric,completed);
            var r=row with {CpuPercent=current,Comparisons=[comparison],RecentComparisons=[],Alert=alert,Alerts=alert==null?[]:[alert]};
            window.RenderSnapshot(basis with {Rows=[r]});window.SelectProcess(r.Key,UiMetric.Cpu);
        }
        Show(normal with {State=MetricState.Unavailable,Threshold=75},MetricState.Learning,null);
        Check(Text("UpperValue")=="학습 중" && Text("CurrentValue")=="측정 불가" && Text("AverageValue")=="50%",
            "display control: completed Learning with finite provisional threshold stays learning under current missing",CardNames.ToDictionary(n=>n,Text));
        Show(normal with {State=MetricState.Unavailable,Threshold=double.NaN},MetricState.Normal,50);
        Check(Text("UpperValue")=="근거 없음" && Text("AverageValue")=="50%" && Text("CurrentValue")=="50%",
            "display control: ready completed window without its own upper does not borrow another upper");
        Show(normal with {State=MetricState.Unavailable,Current=0,Threshold=0},MetricState.Normal,0);
        Check(Text("AverageValue")=="0%" && Text("UpperValue")=="0%" && Text("CurrentValue")=="0%","display control: genuine zero is shown in all three cards");
        var alert=new UiAlert {Metric=UiMetric.Cpu,Average=52,Upper=null,Start=Time(normal.Start).AddMinutes(-3),End=Time(normal.End).AddMinutes(-3),Reason="saved alert without upper"};
        Show(normal,MetricState.Normal,null,alert);
        Check(Text("AverageValue")=="52%" && Text("UpperValue")=="근거 없음" && Text("CurrentValue")=="측정 불가" &&
            Text("AverageTime").Contains(alert.Start.Value.ToLocalTime().ToString("HH:mm:ss")) && Text("UpperTime").Contains(alert.End.Value.ToLocalTime().ToString("HH:mm:ss")),
            "display control: clicked-alert shape retains its own missing upper and its own times");
        Screenshot("control-alert-without-upper");
        Show(normal with {State=MetricState.Unavailable,Current=double.NaN,Threshold=double.NaN,Start=normal.End,End=normal.End+600000000},MetricState.Unavailable,50);
        Check(Text("AverageValue")=="근거 없음" && Text("UpperValue")=="근거 없음","display control: a later unavailable completed window replaces prior mature evidence");
        var empty=row with {Creation=row.Creation+1,Comparisons=[],RecentComparisons=[],Alert=null,Alerts=[]};
        window.RenderSnapshot(basis with {Rows=[empty]});
        Check(new[] {"AverageValue","UpperValue","CurrentValue"}.All(n=>Text(n)==""),"display control: new instance does not keep hidden cards for the old selection");
        window.SelectProcess(empty.Key,UiMetric.Cpu);
        Check(Text("AverageValue")=="근거 없음" && Text("UpperValue")=="근거 없음","display control: selecting the new instance does not restore old completed evidence");
        window.RenderSnapshot(basis with {SessionEpoch=basis.SessionEpoch+1,Rows=[]});
        Check(new[] {"AverageValue","UpperValue","CurrentValue"}.All(n=>Text(n)==""),"display control: session transition clears previous cards");
        Save("display-controls.json",new {syntheticProjectionInputs=true,ordinaryControllerSelection=false,checks=Checks.Count});
    }
    [STAThread] private static int Main(string[] args)
    {
        output=Path.GetFullPath(args[0]);if(Directory.Exists(output))throw new IOException("Use a new result directory");Directory.CreateDirectory(output);
        db=Path.Combine(output,"isolated.sqlite");
        bool flowOnly=args.Contains("--flow-only");
        var assembly=typeof(MainWindow).Assembly;
        string sha=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(assembly.Location)));
        Check(sha.Equals(args[1],StringComparison.OrdinalIgnoreCase),"loaded product assembly matches requested SHA-256",new {assembly.Location,sha});
        try {if(!flowOnly)NativeControls();}catch(Exception e) {Check(false,"native control exception",e.ToString());}
        completed_fixture_mode(0);
        var app=new Application {ShutdownMode=ShutdownMode.OnExplicitShutdown};
        window=new MainWindow {Width=1440,Height=900,ShowActivated=false,ShowInTaskbar=false};
        var options=assembly.GetType("PcMonitor.Options",true)!.GetMethod("Parse")!.Invoke(null,[new[] {"--synthetic","--db",db,"--interval-ms","20","--count","161","--diagnostics",Path.Combine(output,"worker.jsonl")}])!;
        var controllerType=assembly.GetType("PcMonitor.WindowController",true)!;
        var controller=Activator.CreateInstance(controllerType,window,options)!;
        var loop=controllerType.GetField("loop",Private)!.GetValue(controller)!;var loopType=loop.GetType();
        void Stop()=>loopType.GetMethod("Stop")!.Invoke(loop,null);
        var timer=new DispatcherTimer {Interval=TimeSpan.FromSeconds(45)};
        timer.Tick+=(_,_)=> {timer.Stop();Check(false,"runner timeout");Stop();};
        Action<UiSnapshot> published=ui=> {
            try {
                var native=(Snapshot?)loopType.GetField("frame",Private)!.GetValue(loop);
                if(ui.Sequence==121 && !seeded) {Seed(db,50,"ordinary");seeded=true;}
                window.Dispatcher.Invoke(()=> {
                    if(!ReferenceEquals(window.CurrentSnapshot,ui))throw new Exception("Controller did not consume the published frame before observation");
                    ++observedControllerFrames;
                    if(!selected && ui.Rows.Count==1)selected=window.SelectProcess(ui.Rows.Single().Key,UiMetric.Cpu);
                    if(native!=null && new long[] {141,142,143,144,161}.Contains(ui.Sequence) && !Frames.ContainsKey(ui.Sequence)) {
                        string label=ui.Sequence switch {141=>"completed-before",142=>"normal-before",143=>"cpu-missing",144=>"current-recovered",_=>"window-recovered"};
                        Capture(label,native,ui);
                        if(ui.Sequence==161 && !flowOnly)DisplayCases(ui,native.Rows.Single().Metric[0]);
                    }
                },DispatcherPriority.Background);
            }catch(Exception e) {Check(false,"published observation exception",e.ToString());Stop();}
        };
        loopType.GetEvent("Published")!.AddEventHandler(loop,published);
        window.Loaded+=(_,_)=>timer.Start();
        int appExit=app.Run(window);timer.Stop();
        Check(appExit==0,"actual product worker/controller exits normally",appExit);
        Check(observedControllerFrames>=161,"actual controller consumed at least 161 observed publications",observedControllerFrames);
        try {VerifyFlow();}catch(Exception e) {Check(false,"flow verification exception",e.ToString());}
        var module=Process.GetCurrentProcess().Modules.Cast<ProcessModule>().Single(m=>m.ModuleName.Equals("MonitorBridge.dll",StringComparison.OrdinalIgnoreCase));
        string nativePath=module.FileName,nativeHash=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(nativePath)));
        Check(Path.GetFullPath(nativePath).Equals(Path.Combine(AppContext.BaseDirectory,"MonitorBridge.dll"),StringComparison.OrdinalIgnoreCase),"loaded native is the isolated fixture runner copy",new {nativePath,nativeHash});
        Save("results.json",new {failures,checks=Checks.Count,sha,product=assembly.Location,nativePath,nativeHash,flowOnly,
            actualProductWorker=true,actualEngine=true,actualProjection=true,actualMainWindow=true,actualWindowController=true,
            actualConsume=true,actualSyncRecentSelection=true,observedControllerFrames,syntheticPersistedInput=true,realPcData=false,
            ordinaryFlowClickedNotification=false,ordinaryFlowUiRowOverride=false,ordinaryFlowRenderSnapshotOverride=false,results=Checks});
        return failures==0?0:1;
    }
}
