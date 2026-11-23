// 제품 ProgramInfoWindow/Engine/TrayHost를 연결한 worker 수명·동의·만료 회귀다.
// 자기 프로세스의 로컬 수집 뒤 native fixture로 외부 조회 미리보기와 취소를 확인한다.
// 실제 외부 요청은 보내지 않으며 지연 완료가 선택 변경/창 닫기 뒤 채택되지 않는지도 확인한다.
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using PcMonitor;

internal static class Check
{
    private static int _checks;
    private static void Assert(bool condition,string name)
    {
        ++_checks; Console.WriteLine($"{(condition?"PASS":"FAIL")} {name}");
        if(!condition) throw new Exception(name);
    }
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern nint pc_test_fixture();
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void pc_test_research(ProgramInfoHandle handle);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void pc_test_expire(ProgramInfoHandle handle);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int pc_test_tray_stale();
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void pc_test_delay(uint milliseconds);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void pc_test_browser_source(ProgramInfoHandle handle,int trusted);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void pc_test_browser_mode(int mode);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int pc_test_browser_hr();
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern void pc_test_execute_delay(uint milliseconds,int expire);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] private static extern int pc_test_destroy_count();
    private static readonly BindingFlags Private=BindingFlags.NonPublic|BindingFlags.Instance|BindingFlags.DeclaredOnly;
    private static T Field<T>(object obj,string name) => (T)obj.GetType().GetField(name,Private)!.GetValue(obj)!;
    private static void SetField(object obj,string name,object? value) => obj.GetType().GetField(name,Private)!.SetValue(obj,value);
    private static object? Call(object obj,string name,params object[] args) => obj.GetType().GetMethod(name,Private)!.Invoke(obj,args);
    private static T Control<T>(ProgramInfoWindow window,string name) where T: FrameworkElement => (T)window.FindName(name);
    private static void Click(ProgramInfoWindow window,string name) => Control<Button>(window,name).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
    private static async Task IdleUntil(Func<bool> condition)
    {
        var watch=Stopwatch.StartNew();
        while(!condition()) { if(watch.ElapsedMilliseconds>18000) throw new Exception("UI worker timeout"); await Task.Delay(25); }
    }
    [STAThread] private static int Main(string[] args)
    {
        var app=new Application { ShutdownMode=ShutdownMode.OnExplicitShutdown };
        int exit=0;
        app.Startup+=async (_,_) =>
        {
            var owner=new Window { Title="Companion fixture owner",Width=400,Height=300,Left=-5000,Top=-5000,ShowInTaskbar=false,ShowActivated=false };
            ProgramInfoWindow? dialog=null;
            try
            {
                owner.Show(); owner.Hide();
                using var process=Process.GetCurrentProcess();
                ProgramInfoTarget? target=new(7,(uint)process.Id,(ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc(),process.MainModule!.FileName,"Companion Fixture");
                dialog=new ProgramInfoWindow(owner,target,()=>target) { Left=-5000,Top=-5000,ShowInTaskbar=false,ShowActivated=false };
                dialog.Show(); dialog.Hide();
                await IdleUntil(()=>!Field<bool>(dialog,"_busy") && Field<ProgramInfoHandle?>(dialog,"_context") is not null);
                Assert(Control<TextBox>(dialog,"BodyText").Text.Contains("일반 역할"),"real own-process product collect rendered");
                Assert(ProgramInfoNative.Current(Field<ProgramInfoHandle>(dialog,"_context")),"real own-process pid creation path verified");
                Assert(!Control<Button>(dialog,"ApproveButton").IsVisible,"no automatic public consent");

                Field<ProgramInfoHandle>(dialog,"_context").Dispose();
                var fixture=new ProgramInfoHandle(pc_test_fixture());
                SetField(dialog,"_context",fixture); SetField(dialog,"_invalid",false); Call(dialog,"Render");
                using(var cancel=new EventWaitHandle(false,EventResetMode.ManualReset))
                {
                    Assert(ProgramInfoNative.Execute(fixture,0,0,0,cancel.SafeWaitHandle)==0,"native execute rejects absent lookup consent before transmission");
                    Assert(ProgramInfoNative.Text(fixture,0,false).Contains("외부 요청을 보내지 않았습니다"),"native absent-consent rejection states no request");
                    Assert(ProgramInfoNative.Current(fixture),"absent or changed preview does not invalidate otherwise current local evidence");
                }
                Click(dialog,"ResearchButton");
                string preview=Control<TextBox>(dialog,"BodyText").Text;
                Assert(preview.Contains("보내기 전 확인") && preview.Contains("Wikidata") && preview.Contains("Companion%20Fixture"),"public lookup exact values and destination preview");
                Assert(!preview.Contains(target.Path,StringComparison.Ordinal),"private local path excluded from public preview values");
                Assert(ProgramInfoNative.PreviewReady(fixture),"native consent prepared without execute");
                Assert(Control<Button>(dialog,"ApproveButton").Visibility==Visibility.Visible,"explicit lookup action available only after preview");
                Control<RadioButton>(dialog,"ManualPurpose").IsChecked=true;
                Assert(!ProgramInfoNative.PreviewReady(fixture) && Control<Button>(dialog,"ApproveButton").Visibility==Visibility.Collapsed,"purpose change ends pending consent");
                Assert(ProgramInfoNative.Current(fixture),"purpose-only preview change preserves local identity evidence");
                pc_test_research(fixture);
                Click(dialog,"TranslateButton");
                preview=Control<TextBox>(dialog,"BodyText").Text;
                Assert(preview.Contains("api.mymemory.translated.net") && preview.Contains("장기 보관") && preview.Contains("Companion Fixture is a local application"),"separate translation disclosure contains exact sentence and retention");
                Assert((string)Control<Button>(dialog,"ApproveButton").Content=="이 문장 번역","translation requires separate explicit action");
                Click(dialog,"CancelPreviewButton");
                Assert(!ProgramInfoNative.PreviewReady(fixture),"cancel preview revokes native consent");
                // 실제 ExecuteAsync→native 신뢰 URL 검사→COM 초기화까지 간다. 기본
                // fixture는 브라우저를 열지 않으며 STA 계약만 실제 COM으로 검증한다.
                pc_test_browser_source(fixture,0); pc_test_browser_mode(0); Call(dialog,"Render");
                await (Task)Call(dialog,"ExecuteAsync",1)!;
                Assert(pc_test_browser_hr()==unchecked((int)0x8000000A),"untrusted source never reaches browser dispatch");
                Assert(Control<TextBox>(dialog,"BodyText").Text.Contains("문서를 열 수 없습니다"),"untrusted source rejection remains visible");
                pc_test_browser_source(fixture,1); Call(dialog,"Render");
                int shellMode=args.Contains("--shell-open")?2:args.Contains("--shell-connect")?1:0;
                pc_test_browser_mode(shellMode); pc_test_execute_delay(180,0);
                Task browserWork=(Task)Call(dialog,"ExecuteAsync",1)!;
                await Task.Delay(30);
                Assert(!browserWork.IsCompleted && Field<bool>(dialog,"_busy"),"browser work yields to responsive UI dispatcher");
                Assert(!Control<Button>(dialog,"OpenButton").IsEnabled,"browser work keeps concurrent actions disabled");
                await browserWork;
                Console.WriteLine($"BROWSER mode={shellMode} HRESULT=0x{pc_test_browser_hr():X8}");
                Assert(pc_test_browser_hr()>=0,"browser worker uses compatible STA COM apartment");
                Assert(Control<TextBlock>(dialog,"StateBadge").Text=="확인 완료" && Control<Button>(dialog,"ResearchButton").IsEnabled,"current browser completion retains ready evidence");
                pc_test_browser_mode(0); // 별도 실제 Shell probe도 명시한 문서 한 번만 연다.
                ((DispatcherTimer)Field<DispatcherTimer>(dialog,"_reconcileTimer")).Stop();
                string acceptedBody=Control<TextBox>(dialog,"BodyText").Text;
                string acceptedUrl=Control<TextBox>(dialog,"SourceUrl").Text;
                pc_test_execute_delay(50,1);
                int expiredEnables=0;
                Control<Button>(dialog,"ResearchButton").IsEnabledChanged+=(_,eventArgs)=> { if((bool)eventArgs.NewValue) ++expiredEnables; };
                await (Task)Call(dialog,"ExecuteAsync",1)!;
                Assert(Control<TextBlock>(dialog,"StateBadge").Text=="다시 확인 필요" && !Control<Button>(dialog,"ResearchButton").IsEnabled && !Control<Button>(dialog,"OpenButton").IsEnabled,"completion rejects expired evidence before any timer tick");
                Assert(expiredEnables==0,"expired execute never transiently enables lookup during completion rendering");
                Assert(Control<TextBox>(dialog,"BodyText").Text==acceptedBody && Control<TextBox>(dialog,"SourceUrl").Text==acceptedUrl && Control<Button>(dialog,"CopyButton").IsEnabled && Control<TextBlock>(dialog,"StatusText").Text.Contains("만료"),"expired completion preserves previously accepted body and copy URL with current failure reason");
                fixture.Dispose(); fixture=new ProgramInfoHandle(pc_test_fixture());
                SetField(dialog,"_context",fixture); SetField(dialog,"_invalid",false); Call(dialog,"Render");
                pc_test_browser_source(fixture,1); Call(dialog,"Render");
                acceptedBody=Control<TextBox>(dialog,"BodyText").Text;
                pc_test_execute_delay(50,2);
                await (Task)Call(dialog,"ExecuteAsync",1)!;
                Assert(!ProgramInfoNative.Current(fixture) && Control<TextBlock>(dialog,"StateBadge").Text=="다시 확인 필요" && !Control<Button>(dialog,"ResearchButton").IsEnabled,"failed file-stamp recheck invalidates native and managed evidence together");
                Assert(Control<TextBox>(dialog,"BodyText").Text==acceptedBody,"file-stamp failure cannot replace prior body with unaccepted result");
                fixture.Dispose(); fixture=new ProgramInfoHandle(pc_test_fixture());
                SetField(dialog,"_context",fixture); SetField(dialog,"_invalid",false); Call(dialog,"Render");
                Click(dialog,"ResearchButton");
                target=null; Call(dialog,"Reconcile");
                Assert(Field<bool>(dialog,"_invalid") && Control<TextBox>(dialog,"BodyText").Text.Contains("다른 프로세스"),"selection or session loss invalidates accepted evidence");
                Assert(Control<Button>(dialog,"ApproveButton").Visibility==Visibility.Collapsed && !Control<Button>(dialog,"OpenButton").IsEnabled,"selection change blocks external actions");
                target=new(8,(uint)process.Id,(ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc(),process.MainModule!.FileName,"Companion Fixture");
                await (Task)Call(dialog,"RefreshAsync")!;
                Assert(Field<ProgramInfoTarget>(dialog,"_target").Session==8,"refresh binds new session explicitly");
                fixture=Field<ProgramInfoHandle>(dialog,"_context");
                pc_test_expire(fixture); Call(dialog,"Reconcile");
                Assert(!Control<Button>(dialog,"ResearchButton").IsEnabled && Control<TextBlock>(dialog,"StatusText").Text.Contains("만료"),"expired evidence disables new lookup");
                Assert(pc_test_tray_stale()!=0,"old tray icon callback cannot click new event");
                dialog.Close(); dialog=null;

                target=new(9,9999,123,"C:\\fixture\\synthetic.exe","합성",true);
                dialog=new ProgramInfoWindow(owner,target,()=>target) { ShowInTaskbar=false,ShowActivated=false,Left=-5000,Top=-5000 };
                int staleEnables=0;
                Control<Button>(dialog,"ResearchButton").IsEnabledChanged+=(_,eventArgs)=> { if((bool)eventArgs.NewValue) ++staleEnables; };
                dialog.Show(); dialog.Hide();
                Field<DispatcherTimer>(dialog,"_reconcileTimer").Stop();
                await IdleUntil(()=>!Field<bool>(dialog,"_busy") && Field<ProgramInfoHandle?>(dialog,"_context") is not null);
                Assert(Control<TextBox>(dialog,"BodyText").Text.Contains("합성 화면"),"synthetic screen explicitly skips real identity lookup");
                Assert(Control<TextBlock>(dialog,"StateBadge").Text=="다시 확인 필요" && !Control<Button>(dialog,"ResearchButton").IsEnabled && !Control<Button>(dialog,"TranslateButton").IsEnabled,"stale collect completion has invalid badge and disabled actions without timer");
                Assert(staleEnables==0,"stale collect never transiently enables lookup while populating sources");
                Click(dialog,"ResearchButton");
                Assert(Control<Button>(dialog,"ApproveButton").Visibility==Visibility.Collapsed,"synthetic evidence cannot authorize public requests");
                dialog.Width=460; dialog.Height=440; dialog.UpdateLayout();
                var scroll=Control<ScrollViewer>(dialog,"DialogScroll");
                Assert(scroll.ScrollableHeight>0,"short narrow dialog keeps overflowing commands in scrollable content");
                scroll.ScrollToEnd(); dialog.UpdateLayout();
                Assert(scroll.VerticalOffset>0,"short narrow dialog can scroll to bottom actions");
                dialog.Close(); dialog=null;

                target=new(10,(uint)process.Id,(ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc(),process.MainModule!.FileName,"Slow fixture");
                pc_test_delay(350);
                dialog=new ProgramInfoWindow(owner,target,()=>target) { ShowInTaskbar=false,ShowActivated=false,Left=-5000,Top=-5000 };
                dialog.Show(); dialog.Hide();
                await IdleUntil(()=>Field<bool>(dialog,"_busy"));
                target=target with { Session=11 };
                Call(dialog,"Reconcile");
                await IdleUntil(()=>!Field<bool>(dialog,"_busy"));
                Assert(Field<ProgramInfoHandle?>(dialog,"_context") is null,"late old-session local response is discarded");
                Assert(Control<TextBox>(dialog,"BodyText").Text.Contains("다른 프로세스"),"late response cannot overwrite invalidation notice");
                pc_test_delay(350);
                int releasedBefore=pc_test_destroy_count();
                Task closingWork=(Task)Call(dialog,"RefreshAsync")!;
                dialog.Close();
                await closingWork;
                Assert(Field<bool>(dialog,"_closed") && Field<ProgramInfoHandle?>(dialog,"_context") is null,"close during delayed native work drops result and releases handle");
                Assert(pc_test_destroy_count()==releasedBefore+1,"unaccepted late collect result releases exactly once");
                dialog=null;
                // 타이머 없이 완료 경계만으로 세션·PID 재사용·선택 교체를 거른다.
                for(int change=0;change<3;++change)
                {
                    target=new(20,(uint)process.Id,(ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc(),process.MainModule!.FileName,"completion fixture");
                    dialog=new ProgramInfoWindow(owner,target,()=>target) {ShowInTaskbar=false,ShowActivated=false,Left=-5000,Top=-5000};
                    Field<DispatcherTimer>(dialog,"_reconcileTimer").Stop();
                    pc_test_delay(100); Task staleWork=(Task)Call(dialog,"RefreshAsync")!;
                    target=change switch {0=>target with{Session=21},1=>target with{Creation=target.Creation+1},_=>target with{Pid=target.Pid+1}};
                    int released=pc_test_destroy_count(); await staleWork;
                    Assert(Field<ProgramInfoHandle?>(dialog,"_context") is null && !Control<Button>(dialog,"ResearchButton").IsEnabled,$"late collect target change {change} rejected at completion");
                    Assert(pc_test_destroy_count()==released+1,$"late collect target change {change} handle released exactly once");
                    dialog.Close(); dialog=null;
                }
                // 같은 native context를 작업 중 닫아도 SafeHandle P/Invoke 참조가
                // 작업 반환까지 해제를 늦춘다. 취소된 결과는 배지에 수락하지 않는다.
                for(int ending=0;ending<3;++ending)
                {
                    target=new(30,(uint)process.Id,(ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc(),process.MainModule!.FileName,"execute lifetime");
                    dialog=new ProgramInfoWindow(owner,target,()=>target) {ShowInTaskbar=false,ShowActivated=false,Left=-5000,Top=-5000};
                    Field<DispatcherTimer>(dialog,"_reconcileTimer").Stop();
                    fixture=new ProgramInfoHandle(pc_test_fixture()); pc_test_browser_source(fixture,1);
                    SetField(dialog,"_context",fixture); Call(dialog,"Render"); Call(dialog,"SetEnabled");
                    pc_test_execute_delay(250,0); Task lateExecute=(Task)Call(dialog,"ExecuteAsync",1)!;
                    await Task.Delay(50); int released=pc_test_destroy_count();
                    if(ending==0) target=target with{Session=31};
                    else if(ending==1) dialog.CancelPending();
                    else dialog.Close();
                    await lateExecute;
                    if(ending==1) Assert(!ProgramInfoNative.Current(fixture),"native canceled execute requires fresh local evidence");
                    if(ending<2)
                    {
                        Assert(Control<TextBlock>(dialog,"StateBadge").Text=="다시 확인 필요" && !Control<Button>(dialog,"ResearchButton").IsEnabled,$"late execute ending {ending} not accepted");
                        dialog.Close();
                    }
                    Assert(fixture.IsClosed && pc_test_destroy_count()==released+1,$"late execute ending {ending} handle released exactly once");
                    dialog=null;
                }
                using(var cancel=new EventWaitHandle(true,EventResetMode.ManualReset))
                using(var canceled=ProgramInfoNative.Collect(target,cancel.SafeWaitHandle))
                {
                    Assert(!ProgramInfoNative.Current(canceled) && ProgramInfoNative.Text(canceled,0,false).Contains("조회 취소"),"signaled cancel event prevents local evidence collection");
                }
                Console.WriteLine($"RESULT {_checks} checks passed; lookup/translation not executed; actual browser probe={string.Join(',',args)}.");
            }
            catch(Exception error) { exit=1; Console.WriteLine(error.ToString()); }
            finally { dialog?.Close(); owner.Close(); app.Shutdown(exit); }
        };
        app.Run(); return exit;
    }
}
