// 실제 membership/bridge와 WPF worker→controller→창을 함께 검사한다. 할당 실패는
// 시험 DLL의 실제 pg_groups_build 첫 calloc에만 주입하고, UI 스레드에서 native를 읽지 않는다.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal static class GroupRefresh
{
    private const BindingFlags Private=BindingFlags.Instance|BindingFlags.NonPublic;
    private const string Pending="목록 갱신을 기다리는 중입니다.";
    private const string Retention="오래된 기록 정리 지연";
    private const string Saved="알림 설정을 저장했습니다.";
    private static readonly List<object> checks=new(),ledger=new();
    private static readonly JsonSerializerOptions Json=new() {WriteIndented=true,IncludeFields=true};
    private static string output="";
    private static MainWindow window=null!;
    private static object loop=null!,controller=null!;
    private static Type loopType=null!,controllerType=null!;
    private static int failures;
    [StructLayout(LayoutKind.Sequential)] private struct Observation
    {
        public int HasAssignment,Shared,ShownKind,Pending,Failed,GroupingFailed;
        public uint AllocationHits,RemainingFailures,UnexpectedAllocations,LastLine;
        public uint SeparateCalls,SeparateTransitions,BuildCalls,Session;
        public ulong AssignmentId,TargetId,Sequence,ShownId,AssignedCount,ShownCount;
    }
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int group_fixture_observe(IntPtr b,out Observation state);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int group_fixture_arm(IntPtr b,uint failures,int reset);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int group_fixture_edit(IntPtr b,int action,int staleSession);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int group_fixture_sql(IntPtr b,[MarshalAs(UnmanagedType.LPUTF8Str)]string sql);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int bridge_group_refresh_pending(IntPtr b);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern IntPtr bridge_error(IntPtr b);
    private static IntPtr Handle(Engine e)=>(IntPtr)typeof(Engine).GetField("handle",Private)!.GetValue(e)!;
    private static string Error(Engine e)=>Marshal.PtrToStringUTF8(bridge_error(Handle(e)))??"";
    private static Observation Observe(Engine e,string stage)
    {
        if(group_fixture_observe(Handle(e),out var state)!=1)throw new InvalidOperationException("fixture observation rejected");
        ledger.Add(new {stage,state,error=Error(e),thread=Environment.CurrentManagedThreadId});
        return state;
    }
    private static void Arm(Engine e,uint count,bool reset=true)
    {if(group_fixture_arm(Handle(e),count,reset?1:0)!=1)throw new InvalidOperationException("fixture arm rejected");}
    private static void Sql(Engine e,string sql)
    {
        int result=group_fixture_sql(Handle(e),sql);
        ledger.Add(new {stage="fixture-sql",sql,result});
        if(result!=0)throw new InvalidOperationException($"fixture SQL returned {result}: {sql}");
    }
    private static void Check(bool condition,string name,object? detail=null)
    {
        checks.Add(new {name,passed=condition,detail});
        if(!condition) {++failures;throw new InvalidOperationException(name);}
    }
    private static void Until(Func<bool> ready,string name,int milliseconds=8000)
    {
        var time=Stopwatch.StartNew();
        while(!ready()) {
            if(time.ElapsedMilliseconds>milliseconds)throw new TimeoutException(name);
            var frame=new DispatcherFrame();var timer=new DispatcherTimer {Interval=TimeSpan.FromMilliseconds(10)};
            timer.Tick+=(_,_)=>{timer.Stop();frame.Continue=false;};timer.Start();Dispatcher.PushFrame(frame);
        }
    }
    private static string Text(string name)=>((TextBlock)window.FindName(name)).Text;
    private static string Status()=>Text("CommandStatusText");
    private static string Group()=>window.CurrentSnapshot.Groups.Single(g=>g.Key==window.CurrentSnapshot.Rows.Single().GroupKey).Name;
    private static void Act(string name)=>controllerType.GetMethod("Act",Private)!.Invoke(controller,[new UiCommand(name)]);
    private static MenuItem GroupItem(string prefix)
    {
        var menu=(ContextMenu)typeof(MainWindow).GetMethod("MakeGroupMenu",Private)!.Invoke(window,null)!;
        return menu.Items.OfType<MenuItem>().Single(item=>item.Header.ToString()!.StartsWith(prefix,StringComparison.Ordinal));
    }
    private static void CheckPendingGroupMenus(string stage)
    {
        string key=window.CurrentSnapshot.Rows.Single().Key;
        Check(window.SelectGroup(window.CurrentSnapshot.Rows.Single().GroupKey),stage+" selects displayed old group");
        Check(!GroupItem("현재 구성원 확정").IsEnabled && !GroupItem("이 그룹을 이동 대상으로").IsEnabled,
            stage+" disables both displayed-group commands while pending");
        Check(window.SelectProcess(key),stage+" restores current identity selection");
        Check(!GroupItem("현재 구성원 확정").IsEnabled && GroupItem("선택 구성원을 이동 대상으로").IsEnabled &&
            GroupItem("선택 구성원을 대상").IsEnabled && GroupItem("자동 그룹에서 분리").IsEnabled && GroupItem("자동 추정 소속").IsEnabled,
            stage+" preserves individually revalidated commands while blocking group confirmation");
    }
    private static T Worker<T>(Func<Engine,T> work)
    {
        var done=new TaskCompletionSource<T>();
        loopType.GetMethod("Enqueue",Private)!.Invoke(loop,[new Action(()=> {
            try {done.SetResult(work((Engine)loopType.GetField("engine",Private)!.GetValue(loop)!));}
            catch(Exception e) {done.SetException(e);}
        })]);
        Until(()=>done.Task.IsCompleted,"worker command completed");
        return done.Task.GetAwaiter().GetResult();
    }
    private static Observation State(string stage)=>Worker(e=>Observe(e,stage));
    private static void NextSample()
    {
        long sequence=window.CurrentSnapshot.Sequence;
        Worker(e=>{loopType.GetField("nextSample",Private)!.SetValue(loop,0L);return 0;});
        Until(()=>window.CurrentSnapshot.Sequence>sequence || window.CurrentSnapshot.SessionEpoch<0,"requested normal sample published");
    }
    private static void Capture(string stage)
    {
        window.UpdateLayout();var s=window.CurrentSnapshot;
        ledger.Add(new {stage="screen-"+stage,s.SessionEpoch,s.Sequence,s.GroupRefreshPending,s.RetentionPending,
            group=s.Rows.Count==1?Group():"",status=Status()});
        var image=new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth),(int)Math.Ceiling(window.ActualHeight),96,96,PixelFormats.Pbgra32);
        image.Render(window);var png=new PngBitmapEncoder();png.Frames.Add(BitmapFrame.Create(image));
        using var stream=File.Create(Path.Combine(output,stage+".png"));png.Save(stream);
    }

    private static void NativeCases()
    {
        using var e=Engine.Open(Path.Combine(output,"native.sqlite"),true);
        var s=e.Collect();var row=s.Rows.Single();Arm(e,0);
        Check(group_fixture_edit(Handle(e),3,0)==0,"native rejected edit returns NULL");
        var state=Observe(e,"native-rejected");
        Check(state.HasAssignment==0 && state.Pending==0 && state.ShownKind==0,"rejected edit preserves initial membership and view");
        s=e.Edit(GroupAction.ProcessTarget,row.Key);
        state=Observe(e,"native-immediate");
        Check(state.HasAssignment==1 && state.Shared==0 && state.ShownKind==1 && !s.GroupRefreshPending && Error(e)=="",
            "immediate success returns refreshed USER and clears pending/error");
        Arm(e,2);
        Check(group_fixture_edit(Handle(e),4,0)==1,"applied edit returns view when rebuild allocation fails");
        state=Observe(e,"native-partial");
        Check(state.Pending==1 && state.Shared==1 && state.AssignmentId==0 && state.ShownKind==1 && state.Failed==0,
            "partial success preserves USER view and committed SHARED membership without fatal failure");
        Check(state.AllocationHits==1 && state.UnexpectedAllocations==0 && state.SeparateCalls==1 && state.SeparateTransitions==1,
            "exact first group allocation failed after one successful membership transition",state);
        string error=Error(e);
        Check(bridge_group_refresh_pending(Handle(e))==1 && bridge_group_refresh_pending(Handle(e))==1 && Error(e)==error,
            "pending query preserves diagnostics and does not rebuild");
        foreach(var action in new[] {GroupAction.Confirm,GroupAction.GroupTarget,GroupAction.Expand,
            GroupAction.Collapse,GroupAction.ExpandAll,GroupAction.CollapseAll}) {
            var before=Observe(e,"native-before-pending-"+action);bool rejected=false;
            try {e.Edit(action,group:s.Groups.Single().Key);}
            catch(EngineException ex) {rejected=ex.Message.Contains("operation was not applied",StringComparison.Ordinal);}
            state=Observe(e,"native-after-pending-"+action);
            Check(rejected && state.Shared==1 && state.AssignmentId==0 && state.TargetId==before.TargetId &&
                state.Pending==1 && state.ShownKind==1 && state.ShownId==before.ShownId && state.SeparateCalls==1 && state.SeparateTransitions==1,
                "pending native "+action+" rejects stale group without changing membership, target or displayed group",state);
            Check(state.BuildCalls==before.BuildCalls && state.AllocationHits==before.AllocationHits &&
                state.RemainingFailures==before.RemainingFailures,"pending native "+action+" does not rebuild or consume failure injection",state);
        }
        Check(group_fixture_edit(Handle(e),3,1)==0,"stale session edit is rejected while pending");
        state=Observe(e,"native-stale-rejected");
        Check(state.Pending==1 && state.Shared==1 && state.SeparateCalls==1,"rejected later edit does not undo partial success");
        s=e.Feedback(row.Key,FeedbackKind.Cancel);
        Check(s.GroupRefreshPending && s.Sequence==1 && s.Groups.Single().Key.Kind==1,"feedback current-view copy preserves pending without rebuilding");
        s=e.Collect();state=Observe(e,"native-repeat-failure");
        Check(s.GroupRefreshPending && s.GroupingFailed && state.AllocationHits==2 && state.SeparateCalls==1 && state.Shared==1,
            "next failed rebuild preserves pending and never repeats membership edit");
        s=e.Collect();state=Observe(e,"native-collected-recovery");
        Check(!s.GroupRefreshPending && !s.GroupingFailed && state.ShownKind==2 && state.SeparateTransitions==1 && state.AllocationHits==2,
            "next successful collection refreshes SHARED and clears only native refresh flags");
        s=e.Edit(GroupAction.ProcessTarget,row.Key);Arm(e,2);
        s=e.Edit(GroupAction.SeparateShared,row.Key);s=e.Collect();
        Check(s.GroupRefreshPending && s.GroupingFailed,"edit-recovery fixture reached repeated allocation failure");
        ulong beforeEditSequence=s.Sequence;
        s=e.Edit(GroupAction.RestoreAuto,row.Key);state=Observe(e,"native-edit-recovery");
        Check(!s.GroupRefreshPending && !s.GroupingFailed && state.HasAssignment==0 && state.ShownKind==0 && s.Sequence==beforeEditSequence,
            "successful edit rebuild clears grouping failure and pending within the same sequence");
        s=e.Edit(GroupAction.ProcessTarget,row.Key);Arm(e,1);s=e.Edit(GroupAction.SeparateShared,row.Key);
        uint oldSession=s.Session;e.Pause();e.Resume();s=e.Collect();state=Observe(e,"native-new-session");
        Check(s.Session!=oldSession && !s.GroupRefreshPending && state.HasAssignment==0,"resume resets pending and membership for the new session");
    }

    private static void UiCases()
    {
        Until(()=>window.CurrentSnapshot.Rows.Count==1,"initial WPF synthetic sample");
        Check(window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key,UiMetric.Cpu),"select actual synthetic instance");
        Worker(e=>{Arm(e,0);return 0;});
        Act("group-move");Until(()=>Status().Contains("operation was not applied"),"initial membership rejection shown");
        var state=State("ui-rejected");
        Check(state.HasAssignment==0 && !window.CurrentSnapshot.GroupRefreshPending && Group()=="기타","WPF rejected edit preserves initial Other display");
        Act("group-target");Until(()=>Group()=="SYNTHETIC fixture.exe" && Status().Contains("그룹 지정을 적용"),"USER target displayed");
        Check(!window.CurrentSnapshot.GroupRefreshPending,"WPF immediate successful edit has no pending notice");
        Act("group-confirm");Until(()=>Status().Contains("소속 그룹이나"),"previous group rejection displayed");
        Check(window.SelectGroup(window.CurrentSnapshot.Rows.Single().GroupKey),"select valid USER before capturing group menu");
        var oldConfirm=GroupItem("현재 구성원 확정");var oldTarget=GroupItem("이 그룹을 이동 대상으로");
        Check(oldConfirm.IsEnabled && oldTarget.IsEnabled,"both group menu commands enabled before membership edit");
        Check(window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key),"select actual instance for separation");
        Worker(e=>{Arm(e,2);return 0;});
        long sequence=window.CurrentSnapshot.Sequence;
        Act("group-detach");Until(()=>window.CurrentSnapshot.GroupRefreshPending && Status().Contains(Pending) && !Status().Contains("소속 그룹이나"),"partial success published and prior group rejection replaced");
        state=State("ui-partial");
        Check(Group()=="SYNTHETIC fixture.exe" && window.CurrentSnapshot.Sequence==sequence && state.Shared==1 && state.ShownKind==1,
            "WPF immediately publishes refresh pending with prior USER view and actual SHARED membership");
        Check(state.AllocationHits==1 && state.SeparateCalls==1 && state.SeparateTransitions==1 && state.Failed==0,
            "WPF partial edit applies membership exactly once and does not stop collection",state);
        Check(!Status().Contains("operation was not applied"),"partial success is not reported as unapplied");Capture("partial-pending");
        CheckPendingGroupMenus("first pending list");
        // 이미 열어 둔 메뉴는 IsEnabled를 다시 읽기 전에 클릭될 수도 있다. 이전 메뉴의
        // 실제 Click→Request→worker 경로도 native 거절과 실제 소속·대상을 대조한다.
        foreach(var (name,item) in new[] {("confirm",oldConfirm),("group-target",oldTarget)}) {
            Check(window.SelectGroup(window.CurrentSnapshot.Rows.Single().GroupKey),"select old group for delayed "+name+" menu click");
            var before=State("ui-before-old-menu-"+name);window.SetCommandStatus("delayed menu "+name);
            item.RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent));
            Until(()=>Status().Contains("operation was not applied"),"delayed old "+name+" rejected by native");
            state=State("ui-after-old-menu-"+name);
            Check(window.CurrentSnapshot.GroupRefreshPending && Status().Contains(Pending) && state.Shared==1 && state.AssignmentId==0 &&
                state.TargetId==before.TargetId && state.ShownKind==1 && state.ShownId==before.ShownId && state.SeparateCalls==1 &&
                state.SeparateTransitions==1 && state.BuildCalls==before.BuildCalls && state.AllocationHits==before.AllocationHits,
                "delayed old "+name+" menu reports rejection and preserves actual membership, target, pending and one edit",state);
        }
        Check(window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key),"restore individual selection after delayed group commands");

        var same=window.CurrentSnapshot;
        ((TextBox)window.FindName("SearchBox")).Text="SYNTHETIC";((TextBox)window.FindName("SearchBox")).Clear();
        ((ComboBox)window.FindName("SortBox")).SelectedIndex=2;
        window.RenderSnapshot(same);window.RenderSnapshot(same);
        controllerType.GetMethod("Tick",Private)!.Invoke(controller,null);
        var wait=Stopwatch.StartNew();Until(()=>wait.ElapsedMilliseconds>=1100,"actual maintenance timer elapsed");
        Check(window.CurrentSnapshot.GroupRefreshPending && window.CurrentSnapshot.Sequence==sequence && Status().Contains(Pending),
            "search sort same-frame rendering and maintenance timer retain pending");
        Act("toggle-notifications");Until(()=>!window.CurrentSnapshot.NotificationsEnabled,"notification setting republished");
        Check(window.CurrentSnapshot.GroupRefreshPending && Status().Contains(Pending),"non-group same-frame republish retains pending");

        Worker(e=>{Sql(e,"INSERT INTO feedback(program_id,kind,created,expires) SELECT id,1,0,1 FROM program;"+
            "CREATE TRIGGER group_stop_trim BEFORE DELETE ON feedback BEGIN SELECT RAISE(ABORT,'group fixture retention'); END;");return 0;});
        Act("feedback-suppress-hour");Until(()=>Status().Contains(Saved) && window.CurrentSnapshot.RetentionPending,"feedback committed while cleanup is delayed");
        Check(window.CurrentSnapshot.GroupRefreshPending && Status().Contains(Pending) && Status().Contains(Retention),
            "feedback republish preserves refresh pending alongside independent storage warning");
        state=State("ui-feedback-republish");
        Check(state.AllocationHits==1 && state.SeparateCalls==1 && state.BuildCalls==1,"feedback and redraws did not retry group construction or reapply membership",state);
        Capture("feedback-and-retention");

        NextSample();state=State("ui-repeat-failure");
        Check(window.CurrentSnapshot.GroupRefreshPending && state.GroupingFailed==1 && state.AllocationHits==2 && state.SeparateTransitions==1,
            "repeated collection rebuild failure retains pending and one membership transition",state);
        Check(Status().Contains(Pending) && Status().Contains(Saved) && Status().Contains(Retention),
            "repeat failure preserves independent command result and retention warning");
        CheckPendingGroupMenus("repeated failed list");
        NextSample();state=State("ui-recovery");
        Check(!window.CurrentSnapshot.GroupRefreshPending && Group()=="기타" && state.GroupingFailed==0 && state.SeparateCalls==1,
            "next normal WPF list shows Other and clears pending without reapplying edit",state);
        Check(!Status().Contains(Pending) && Status().Contains(Saved) && Status().Contains(Retention),
            "group recovery removes only pending notice and preserves feedback result and retention warning");Capture("recovered-with-other-status");

        Act("group-target");Until(()=>Group()=="SYNTHETIC fixture.exe","new USER for edit recovery");
        Check(window.SelectGroup(window.CurrentSnapshot.Rows.Single().GroupKey),"select refreshed group after recovery");
        Check(GroupItem("현재 구성원 확정").IsEnabled && GroupItem("이 그룹을 이동 대상으로").IsEnabled,
            "normal rebuild re-enables both valid group menu commands");
        var beforeValidGroupEdits=State("ui-before-valid-group-after-recovery");
        window.SetCommandStatus("waiting for recovered group confirmation");
        GroupItem("현재 구성원 확정").RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent));
        Until(()=>Status().Contains("이번 세션의 그룹 지정을 적용"),"valid confirmation succeeds after recovery");
        window.SetCommandStatus("waiting for recovered group target");
        GroupItem("이 그룹을 이동 대상으로").RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent));
        Until(()=>Status().Contains("이번 세션의 그룹 지정을 적용"),"valid group target succeeds after recovery");
        state=State("ui-valid-group-after-recovery");
        Check(!window.CurrentSnapshot.GroupRefreshPending && !Status().Contains(Pending) && state.Shared==0 &&
            state.AssignmentId==state.TargetId && state.ShownId==state.TargetId && state.BuildCalls==beforeValidGroupEdits.BuildCalls+2,
            "valid group target succeeds after recovery and corresponds to actual USER membership and target",state);
        Check(window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key),"restore individual selection after valid group edits");
        Worker(e=>{Arm(e,2);return 0;});Act("group-detach");Until(()=>window.CurrentSnapshot.GroupRefreshPending,"second partial success");
        NextSample();Check(State("ui-before-edit-recovery").GroupingFailed==1,"second pending scenario has actual failed reconstruction");
        long beforeEditSequence=window.CurrentSnapshot.Sequence;
        Act("group-auto");Until(()=>!window.CurrentSnapshot.GroupRefreshPending,"successful edit refresh recovered");
        state=State("ui-edit-recovery");
        Check(state.GroupingFailed==0 && state.HasAssignment==0 && Group()=="기타" && window.CurrentSnapshot.Sequence==beforeEditSequence,
            "successful edit reconstructs same-sequence list and clears refresh and grouping failure");

        Act("group-target");Until(()=>Group()=="SYNTHETIC fixture.exe","USER before pending session transition");
        Worker(e=>{Arm(e,1);return 0;});Act("group-detach");Until(()=>window.CurrentSnapshot.GroupRefreshPending,"pending before pause");
        long oldSession=window.CurrentSnapshot.SessionEpoch;
        Act("pause");Until(()=>window.CurrentSnapshot.Paused,"paused with pending");
        Check(window.CurrentSnapshot.GroupRefreshPending && Status().Contains(Pending),"pause does not falsely finish pending refresh");
        Act("resume");Until(()=>window.CurrentSnapshot.SessionEpoch!=oldSession && window.CurrentSnapshot.Rows.Count==1,"new session sample");
        state=State("ui-new-session");
        Check(!window.CurrentSnapshot.GroupRefreshPending && !Status().Contains(Pending) && state.HasAssignment==0,
            "new session discards old membership and pending notice");
        window.SetCommandStatus("다른 명령 결과 · 보존 검사");
        typeof(MainWindow).GetMethod("SetGroupCommandStatus",Private)!.Invoke(window,[(uint)oldSession,"stale group callback"]);
        Check(Status().Contains("다른 명령 결과") && !Status().Contains("stale group callback"),"late prior-session group callback cannot replace current command result");
        typeof(MainWindow).GetMethod("SetGroupCommandStatus",Private)!.Invoke(window,[(uint)window.CurrentSnapshot.SessionEpoch,""]);
        Check(Status().Contains("다른 명령 결과"),"empty group result cannot erase independent general status");

        Check(window.SelectProcess(window.CurrentSnapshot.Rows.Single().Key,UiMetric.Cpu),"select new-session instance for fatal case");
        Act("group-target");Until(()=>Group()=="SYNTHETIC fixture.exe","USER before fatal case");
        Worker(e=>{Arm(e,1);return 0;});Act("group-detach");Until(()=>window.CurrentSnapshot.GroupRefreshPending,"pending before real commit failure");
        Worker(e=>{Sql(e,"CREATE TABLE group_bad_fk(p INTEGER REFERENCES program(id) DEFERRABLE INITIALLY DEFERRED);"+
            "CREATE TRIGGER group_fail_commit AFTER INSERT ON collection_run BEGIN INSERT INTO group_bad_fk VALUES(-1); END;");return 0;});
        NextSample();Until(()=>window.CurrentSnapshot.SessionEpoch<0 && Status().Contains("FOREIGN KEY"),"real collection COMMIT failure shown");
        state=State("ui-fatal");
        Check(state.Failed==1 && state.Pending==1 && Status().Contains("중단") && !Status().Contains(Pending) && !Status().Contains(Retention),
            "fatal collection reason takes precedence over old pending and retention while native state remains pending");
        Capture("fatal-precedence");
    }

    [STAThread] private static int Main(string[] args)
    {
        if(args.Length!=2)return 2;
        output=Path.GetFullPath(args[0]);if(Directory.Exists(output))return 2;Directory.CreateDirectory(output);
        string sha=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(typeof(MainWindow).Assembly.Location)));
        string nativeSha=Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory,"MonitorBridge.dll"))));
        try {
            Check(sha.Equals(args[1],StringComparison.OrdinalIgnoreCase),"loaded managed assembly matches tested product SHA",sha);
            NativeCases();
            var app=new Application {ShutdownMode=ShutdownMode.OnExplicitShutdown};
            var assembly=typeof(MainWindow).Assembly;
            object options=assembly.GetType("PcMonitor.Options",true)!.GetMethod("Parse")!.Invoke(null,[new[] {
                "--synthetic","--db",Path.Combine(output,"wpf.sqlite"),"--interval-ms","60000","--diagnostics",Path.Combine(output,"worker.jsonl")}])!;
            window=new MainWindow {Width=1440,Height=900,ShowActivated=false,ShowInTaskbar=false,Left=-32000,Top=-32000};
            controllerType=assembly.GetType("PcMonitor.WindowController",true)!;controller=Activator.CreateInstance(controllerType,window,options)!;
            loop=controllerType.GetField("loop",Private)!.GetValue(controller)!;loopType=loop.GetType();
            window.Loaded+=(_,_)=>window.Dispatcher.BeginInvoke(new Action(()=> {
                try {UiCases();}
                catch(Exception e) {++failures;ledger.Add(new {stage="unexpected",error=e.ToString(),status=Status()});Console.Error.WriteLine(e);}
                finally {window.Close();}
            }));
            app.Run(window);
        } catch(Exception e) {++failures;ledger.Add(new {stage="setup-or-native-failure",error=e.ToString()});Console.Error.WriteLine(e);}
        File.WriteAllText(Path.Combine(output,"results.json"),JsonSerializer.Serialize(new {
            checks=checks.Count,failures,managedSha256=sha,nativeSha256=nativeSha,
            actualProductCode=true,actualWorkerControllerWindow=true,realSystemOom=false,syntheticInput=true,
            normalIntervalMilliseconds=60000,samplesAdvancedByWorkerQueue=true,results=checks,ledger},Json));
        Console.WriteLine($"group refresh checks={checks.Count} failures={failures}");
        return failures==0?0:1;
    }
}
