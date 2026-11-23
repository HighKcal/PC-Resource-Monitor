// 실제 Windows WPF의 worker → controller → native → 시험 DB 경로에서 최근 비교의
// 4/5 준비와 완료 전환을 확인한다. DisplayCases의 장기·혼합·알림 반례는 불변 UI
// 입력만 바꾼 합성 표시 시험이다. 실제 Windows 배너 클릭이나 실사용 이력은 아니다.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal static class RecentComparisonRegression
{
    private const BindingFlags Private=BindingFlags.Instance|BindingFlags.NonPublic;
    private static readonly List<object> Checks=new();
    private static int failures;
    private static string output="";
    private static MainWindow window=null!;
    private static string Text(string n)=>((TextBlock)window.FindName(n)).Text;
    private static void Check(bool ok,string name,object? detail=null) {Checks.Add(new {name,passed=ok,detail});if(!ok)++failures;}
    private static void Capture(string name)
    {
        window.UpdateLayout();
        var bitmap=new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth),(int)Math.Ceiling(window.ActualHeight),96,96,PixelFormats.Pbgra32);
        bitmap.Render(window);var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream=File.Create(Path.Combine(output,name+".png"));png.Save(stream);
    }
    private static UiComparison Long(UiMetric metric,DateTimeOffset end)=>new() {
        Metric=metric,StateCode=MetricState.Normal,Reason="long synthetic fixture",Average=.9e9,Median=.6e9,Upper=1.2e9,
        Start=end.AddMinutes(-1),End=end
    };
    // 코드값만 바꾸어 학습·결측·준비를 구분한다. 표시 문구는 이 시험의 분기나 기대값
    // 선택에 쓰지 않는다. 원본 learning은 실제 worker/native의 첫날 입력에서 가져온다.
    private static void StateCases(UiSnapshot basis)
    {
        var original=basis.Rows.Single();var end=basis.ObservedAt!.Value;
        var learning=original.Comparisons.Single(c=>c.Metric==UiMetric.Cpu);
        Check(learning.StateCode==MetricState.Learning && !learning.Ready && !learning.Candidate,
            "native learning state reaches the UI as one typed state",new {learning.StateCode,learning.State,learning.Ready,learning.Candidate});
        void Show(UiComparison comparison,double? current=7,UiAlert? alert=null)
        {
            var row=original with {CpuPercent=current,RecentComparisons=[],Comparisons=[comparison],Alert=alert,Alerts=alert==null?[]:[alert]};
            window.RenderSnapshot(basis with {Rows=[row]});window.SelectProcess(row.Key,UiMetric.Cpu);
        }
        Show(learning);
        Check(Text("UpperValue")=="학습 중" && Text("CurrentValue")=="7%",
            "learning without recent evidence is selected by its state code",new {label=learning.State,upper=Text("UpperValue")});
        var unavailable=learning with {StateCode=MetricState.Unavailable,CompletedStateCode=MetricState.Unavailable,Reason="측정 결측 · 학습 자료 확인 불가"};
        Show(unavailable,null);
        Check(!unavailable.Ready && !unavailable.Candidate && Text("UpperValue")=="근거 없음" &&
            Text("CurrentValue")=="측정 불가" && Text("CurrentTime").Contains("값 없음"),
            "unavailable stays distinct from learning and valid zero",new {label=unavailable.State,upper=Text("UpperValue"),current=Text("CurrentValue")});
        var ready=Long(UiMetric.Cpu,end) with {Average=8,Median=6,Upper=30};
        Show(ready);
        Check(ready.Ready && !ready.Candidate && Text("AverageValue")=="8%" && Text("UpperValue")=="30%",
            "ready long-term code preserves the completed window upper");
        var attention=ready with {StateCode=MetricState.Attention};
        var candidate=ready with {StateCode=MetricState.Candidate};
        Check(attention.Ready && !attention.Candidate && candidate.Ready && candidate.Candidate,
            "ready and candidate getters derive from the same metric state");
        var alert=new UiAlert {Metric=UiMetric.Cpu,Average=52,Upper=null,Start=end.AddMinutes(-4),End=end.AddMinutes(-3),Reason="saved event without upper"};
        Show(learning,7,alert);
        Check(Text("AverageValue")=="52%" && Text("UpperValue")=="근거 없음" && Text("CurrentValue")=="7%" &&
            Text("AverageTime").Contains(alert.Start!.Value.ToLocalTime().ToString("HH:mm:ss")) &&
            Text("UpperTime").Contains(alert.End!.Value.ToLocalTime().ToString("HH:mm:ss")) &&
            Text("CurrentTime").Contains(original.ObservedAt!.Value.ToLocalTime().ToString("HH:mm:ss")),
            "missing saved alert upper never becomes learning or borrows a later observation",
            new {average=Text("AverageValue"),upper=Text("UpperValue"),averageTime=Text("AverageTime"),upperTime=Text("UpperTime"),currentTime=Text("CurrentTime")});
    }
    // 선택을 잃은 상태에서는 숨겨진 카드도 비워야 한다. 뒤의 선택이 이전 값이나
    // 초과 강조를 재사용하지 않는지 보며, 관련 없는 레이아웃 구조는 고정하지 않는다.
    private static void CheckEmptyAnalysis(string name)
    {
        Check(((FrameworkElement)window.FindName("Analysis")).Visibility!=Visibility.Visible &&
            new[] {"AverageValue","UpperValue","CurrentValue"}.All(n=>Text(n).Length==0) &&
            ((Border)window.FindName("AverageCard")).Tag==null && ((Border)window.FindName("CurrentCard")).Tag==null &&
            ((Image)window.FindName("AnalysisBell")).Visibility!=Visibility.Visible &&
            ((TextBlock)window.FindName("RecentComparisonNote")).Visibility!=Visibility.Visible,
            name,new {average=Text("AverageValue"),upper=Text("UpperValue"),current=Text("CurrentValue"),
                averageTag=((Border)window.FindName("AverageCard")).Tag,currentTag=((Border)window.FindName("CurrentCard")).Tag});
    }
    private static void DisplayCases(UiSnapshot basis)
    {
        StateCases(basis);
        var row=basis.Rows.Single();var end=basis.ObservedAt!.Value;
        var comparisons=row.Comparisons.Select(c=>c.Metric==UiMetric.Ram?Long(UiMetric.Ram,end):c).ToArray();
        row=row with {Comparisons=comparisons};
        var data=basis with {Rows=[row]};
        window.RenderSnapshot(data);window.SelectProcess(row.Key,UiMetric.Ram);window.UpdateLayout();
        Check(Text("UpperTitle")=="구간 당시 개인 상한" && Text("UpperValue")=="1.20 GB" && Text("AnalysisReason")=="long synthetic fixture","mixed metric long baseline takes priority over available recent RAM",new {title=Text("UpperTitle"),value=Text("UpperValue")});
        Capture("long-priority");
        window.SelectProcess(row.Key,UiMetric.Cpu);window.UpdateLayout();
        Check(Text("AnalysisReason")=="최근 기록과 비교","other learning metric still shows recent comparison");
        Capture("mixed-cpu-recent");
        var longCpu=Long(UiMetric.Cpu,end) with {Average=8,Median=6,Upper=30};
        var reversed=basis.Rows.Single() with {Comparisons=basis.Rows.Single().Comparisons.Select(c=>c.Metric==UiMetric.Cpu?longCpu:c).ToArray()};
        window.RenderSnapshot(basis with {Rows=[reversed]});window.SelectProcess(reversed.Key,UiMetric.Cpu);
        Check(Text("AverageValue")=="8%" && Text("UpperValue")=="30%" && Text("UpperTitle")=="구간 당시 개인 상한",
            "CPU long baseline is selected independently from RAM recent evidence");
        window.SelectProcess(reversed.Key,UiMetric.Ram);
        Check(Text("AnalysisReason")=="최근 기록과 비교" && Text("AverageTitle")=="최근 5개 구간 중앙값",
            "RAM keeps recent evidence when the same row has a mature CPU baseline");
        Capture("mixed-cpu-long-ram-recent");
        window.RenderSnapshot(data with {Paused=true});window.SelectProcess(row.Key,UiMetric.Cpu);
        var cpuRecent=row.RecentComparisons.Single(r=>r.Metric==UiMetric.Cpu);
        Check(Text("UpperTitle")=="마지막 유효 구간 평균" && Text("UpperTime").Contains(cpuRecent.End!.Value.ToLocalTime().ToString("HH:mm:ss")),
            "paused recent comparison retains actual interval time without just-ended wording",Text("UpperTime"));
        Capture("paused-last-window");
        var stale=row.RecentComparisons.Select(r=>r.Metric==UiMetric.Cpu?r with {Start=end.AddMinutes(-11),End=end.AddMinutes(-10)}:r).ToArray();
        window.RenderSnapshot(data with {Rows=[row with {RecentComparisons=stale}]});
        Check(Text("UpperTitle")=="마지막 유효 구간 평균" && Text("UpperTime").Contains(end.AddMinutes(-10).ToLocalTime().ToString("HH:mm:ss")),"older valid anchor is labeled with actual time, not just ended",Text("UpperTime"));
        Capture("older-valid-window");
        var alert=new UiAlert {Metric=UiMetric.Cpu,Average=52,Upper=12,Start=end.AddMinutes(-4),End=end.AddMinutes(-3),Reason="saved event",Evidence="copied event fixture"};
        var alerted=row with {Alert=alert,Alerts=[alert]};
        window.RenderSnapshot(data with {Rows=[alerted]});window.SelectProcess(row.Key,UiMetric.Cpu);window.UpdateLayout();
        Check(Text("AverageTitle")=="당시 구간 평균" && Text("UpperTitle")=="당시 평소 상한" && Text("AverageValue")=="52%" && Text("UpperValue")=="12%","copied notification evidence precedes recent and later comparison",new {average=Text("AverageValue"),upper=Text("UpperValue"),current=Text("CurrentValue")});
        Capture("alert-evidence-first");
        Check((string?)((Border)window.FindName("AverageCard")).Tag=="exceeded","complete exceeded alert activates only its saved evidence highlight");
        // 당시 상한만 빠진 알림은 평균·시간은 그대로 남기고 상한은 근거 없음이어야 한다.
        // 최신 개인 상한을 일부라도 끼워 넣으면 다른 관측 구간의 값이 섞이기 때문이다.
        var partial=alert with {Upper=null};
        window.RenderSnapshot(data with {Rows=[row with {Alert=partial,Alerts=[partial],Comparisons=[longCpu]}]});
        Check(Text("AverageValue")=="52%" && Text("UpperValue")=="근거 없음" &&
            Text("AverageTime").Contains(alert.Start!.Value.ToLocalTime().ToString("HH:mm:ss")) &&
            Text("UpperTime").Contains(alert.End!.Value.ToLocalTime().ToString("HH:mm:ss")) &&
            Text("CurrentValue")=="50%" && Text("CurrentTime").Contains(row.ObservedAt!.Value.ToLocalTime().ToString("HH:mm:ss")),
            "partial alert keeps saved average and times without borrowing a ready latest upper",
            new {average=Text("AverageValue"),upper=Text("UpperValue"),averageTime=Text("AverageTime"),upperTime=Text("UpperTime"),current=Text("CurrentValue"),currentTime=Text("CurrentTime")});
        Check(((Border)window.FindName("AverageCard")).Tag==null && ((Image)window.FindName("AnalysisBell")).Visibility==Visibility.Visible,
            "missing alert upper clears exceeded highlight but retains alert identity");
        Capture("alert-missing-upper");
        var missing=alert with {Average=null,Upper=null,Start=null,End=null};
        window.RenderSnapshot(data with {Rows=[row with {Alert=missing,Alerts=[missing]}]});
        Check(Text("AverageValue")=="근거 없음" && Text("UpperValue")=="근거 없음","incomplete alert never borrows recent values");
        // 유효한 0은 측정값이다. 완료 평균의 결측은 별도 대기 상태이며 0으로 바꾸지 않는다.
        var zero=row.RecentComparisons.Select(r=>r.Metric==UiMetric.Cpu?r with {Median=0,Average=0}:r).ToArray();
        window.RenderSnapshot(data with {Rows=[row with {CpuPercent=0,RecentComparisons=zero}]});
        Check(Text("AverageValue")=="0%" && Text("UpperValue")=="0%" && Text("CurrentValue")=="0%",
            "valid zero remains a recent median, completed mean and current measurement");
        var absent=zero.Select(r=>r.Metric==UiMetric.Cpu?r with {Average=null}:r).ToArray();
        window.RenderSnapshot(data with {Rows=[row with {CpuPercent=null,RecentComparisons=absent}]});
        Check(Text("AverageValue")=="준비 중" && Text("UpperValue")=="완료 구간 대기" && Text("CurrentValue")=="측정 불가" && Text("CurrentTime").Contains("값 없음"),
            "missing completed and current values never become valid zero");
        window.RenderSnapshot(data with {Rows=[alerted]});window.SelectGroup(data.Groups.Single().Key);
        Check(((TextBlock)window.FindName("RecentComparisonNote")).Visibility!=Visibility.Visible && !Text("AnalysisReason").Contains("최근 기록"),"group sum never receives recent comparison");
        Check(Text("AverageTitle")=="그룹 CPU" && ((Border)window.FindName("AverageCard")).Tag==null &&
            ((Border)window.FindName("CurrentCard")).Tag==null && ((Image)window.FindName("AnalysisBell")).Visibility!=Visibility.Visible,
            "group transition resets alert and current-card highlights");
        window.RenderSnapshot(data with {Rows=[alerted]});window.SelectProcess(row.Key,UiMetric.Cpu);
        window.RenderSnapshot(data with {Sequence=data.Sequence+1,Rows=[alerted with {IdentityValid=false}]});
        CheckEmptyAnalysis("identity uncertainty hides and clears the previous alert cards");
        window.RenderSnapshot(data with {Rows=[alerted]});window.SelectProcess(row.Key,UiMetric.Cpu);
        window.RenderSnapshot(data with {SessionEpoch=data.SessionEpoch+1});
        CheckEmptyAnalysis("session selection reset clears all previous card values and highlights");
        window.RenderSnapshot(data);window.SelectProcess(row.Key,UiMetric.Ram);
        Check(Text("AverageValue")=="0.90 GB" && Text("UpperValue")=="1.20 GB" && ((Border)window.FindName("AverageCard")).Tag==null,
            "new selection after an empty detail renders only its own evidence");
        var replacement=row with {Creation=row.Creation+1,RecentComparisons=[],Comparisons=[]};
        window.RenderSnapshot(data with {Sequence=data.Sequence+1,Rows=[replacement]});window.SelectProcess(replacement.Key,UiMetric.Cpu);
        Check(!Text("AnalysisReason").Contains("최근 기록") && Text("AverageValue")!="52%","PID reused instance does not inherit selected recent or alert values");
        window.RenderSnapshot(data with {SessionEpoch=data.SessionEpoch+1});
        Check(window.SelectedRow==null,"session restart clears individual selection");
    }
    [STAThread] private static int Main(string[] args)
    {
        output=Path.GetFullPath(args[0]);Directory.CreateDirectory(output);
        var assembly=typeof(MainWindow).Assembly;
        string sha=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(assembly.Location)));
        Check(sha.Equals(args[1],StringComparison.OrdinalIgnoreCase),"loaded product assembly SHA matches",new {path=assembly.Location,sha});
        var app=new Application {ShutdownMode=ShutdownMode.OnExplicitShutdown};
        object options=assembly.GetType("PcMonitor.Options",true)!.GetMethod("Parse")!.Invoke(null,[new[] {"--synthetic","--db",Path.Combine(output,"isolated.sqlite"),"--interval-ms","20","--diagnostics",Path.Combine(output,"worker.jsonl")}])!;
        window=new MainWindow {Width=1440,Height=900,ShowActivated=false,ShowInTaskbar=false};
        var ct=assembly.GetType("PcMonitor.WindowController",true)!;
        object controller=Activator.CreateInstance(ct,window,options)!;
        object loop=ct.GetField("loop",Private)!.GetValue(controller)!;var lt=loop.GetType();
        long Requests()=>(long)lt.GetField("recentRequests",Private)!.GetValue(loop)!;
        void Act(string name)=>ct.GetMethod("Act",Private)!.Invoke(controller,[new UiCommand(name)]);
        void Select(UiSnapshot s,UiRow? r)=>lt.GetMethod("SelectRecent")!.Invoke(loop,[s,r]);
        var timer=new DispatcherTimer {Interval=TimeSpan.FromMilliseconds(20)};var clock=Stopwatch.StartNew();
        int stage=0;long requests=0,stageAt=0;bool sawFour=false,finished=false;
        UiSnapshot? ready=null;
        void Finish(Exception? error=null)
        {
            if(finished)return;finished=true;timer.Stop();if(error!=null)Check(false,"unexpected exception",error.ToString());
            File.WriteAllText(Path.Combine(output,"results.json"),JsonSerializer.Serialize(new {failures,checks=Checks.Count,product=assembly.Location,sha,elapsedMs=clock.ElapsedMilliseconds,actualWpf=true,actualNativeWorker=true,realPcData=false,actualShellClick=false,results=Checks},new JsonSerializerOptions {WriteIndented=true}));
            Console.WriteLine($"recent WPF checks={Checks.Count} failures={failures}");window.Close();
        }
        timer.Tick+=(_,_)=> {
            try {
                var s=window.CurrentSnapshot;
                if(clock.ElapsedMilliseconds>30000) {Finish(new TimeoutException($"stage {stage}; {Text("CommandStatusText")}"));return;}
                if(stage==0 && s.Sequence>=5 && s.Rows.Count==1) {
                    Check(Requests()==0,"unselected list collection makes no recent request",Requests());
                    window.SelectGroup(s.Groups.Single().Key);
                    ((TextBox)window.FindName("SearchBox")).Text="SYNTHETIC";((TextBox)window.FindName("SearchBox")).Clear();
                    ((ComboBox)window.FindName("SortBox")).SelectedIndex=2;
                    window.RenderSnapshot(s);requests=Requests();stageAt=clock.ElapsedMilliseconds;stage=1;
                } else if(stage==1 && clock.ElapsedMilliseconds-stageAt>100) {
                    Check(Requests()==requests && requests==0,"group selection, sort, search and repeated render make no recent request",Requests());
                    window.SelectProcess(s.Rows.Single().Key,UiMetric.Ram);stage=2;
                } else if(stage==2 && window.SelectedRow is { } row && row.RecentComparisons.Count==4) {
                    var ram=row.RecentComparisons.Single(r=>r.Metric==UiMetric.Ram);
                    if(ram.Count==4 && !sawFour) {
                        sawFour=true;Check(ram.State==RecentComparisonState.Preparing && Text("AverageValue")=="준비 중" && Text("RecentComparisonNote").Contains("4/5"),"four past records plus latest window remains preparation",Text("RecentComparisonNote"));Capture("preparing-four");
                    }
                    if(ram.State==RecentComparisonState.Ready && ram.Count>=5) {
                        Check(sawFour,"observed actual native four-to-five transition");
                        Check(ram.Count==5 && Text("AnalysisReason")=="최근 기록과 비교" && Text("AverageTitle")=="최근 5개 구간 중앙값" && Text("UpperTitle")=="방금 끝난 구간 평균","five separate past records unlock real WPF comparison",new {ram.Count,title=Text("AverageTitle"),average=Text("UpperValue")});
                        Check(Text("AverageValue")=="0.07 GB" && Text("UpperValue")=="0.07 GB" && Text("CurrentTitle")=="현재 표본 RAM","recent median, completed mean and raw value use consistent units and distinct labels");
                        Check(row.RecentComparisons.Single(r=>r.Metric==UiMetric.Read).Median==0 && row.RecentComparisons.Single(r=>r.Metric==UiMetric.Write).Average==0,"valid IO zeros survive native bridge and managed projection");
                        Check(((Border)window.FindName("AverageCard")).Tag==null && ((Image)window.FindName("AnalysisBell")).Visibility!=Visibility.Visible,"recent comparison adds no attention color or alert icon");
                        Check(Text("AverageTime").Contains("–") && Text("UpperTime").Contains("60초"),"actual history range and completed duration displayed",new {history=Text("AverageTime"),current=Text("UpperTime")});
                        Capture("recent-five");ready=s;Act("pause");stage=3;
                    }
                } else if(stage==3 && s.Paused) {
                    requests=Requests();Act("toggle-notifications");Act("toggle-notifications");stageAt=clock.ElapsedMilliseconds;stage=4;
                } else if(stage==4 && clock.ElapsedMilliseconds-stageAt>100) {
                    Check(Requests()==requests,"same sequence notification publications reuse selected result",new {before=requests,after=Requests()});
                    var selected=s.Rows.Single();Select(s with {SessionEpoch=s.SessionEpoch+1},selected);Select(s,selected with {Path=selected.Path+".other"});stageAt=clock.ElapsedMilliseconds;stage=5;
                } else if(stage==5 && clock.ElapsedMilliseconds-stageAt>100) {
                    Check(Requests()==requests,"stale session and changed path are rejected before recent query",new {before=requests,after=Requests()});
                    Select(s,s.Rows.Single());stageAt=clock.ElapsedMilliseconds;stage=6;
                } else if(stage==6 && clock.ElapsedMilliseconds-stageAt>100) {
                    Check(Requests()==requests+1,"returning to valid selection reads once even at same snapshot",new {before=requests,after=Requests()});
                    Check(s.Rows.Single().RecentComparisons.Count==4 && !Text("CommandStatusText").Contains("최근 기록 비교를 확인할 수 없습니다"),"selection while paused returns copied recent evidence without a read error",Text("CommandStatusText"));
                    DisplayCases(ready!);Finish();
                }
            } catch(Exception e) {Finish(e);}
        };
        window.Loaded+=(_,_)=>timer.Start();app.Run(window);return failures==0?0:1;
    }
}
