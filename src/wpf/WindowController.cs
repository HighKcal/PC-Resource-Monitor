using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace PcMonitor;

// UI 스레드에서 화면·선택·tray를 조율한다. worker의 최신 복사본은 Publish → Consume,
// 알림 클릭은 Navigate → WithClickedEvidence → MainWindow 표시 선택 순서로 읽는다.
// Engine 접근은 MonitorLoop에 맡기고 이 파일에서 직접 저장·최근 이력을 조회하지 않는다.
internal sealed class WindowController
{
    private readonly MainWindow window;
    private readonly Options options;
    private readonly MonitorLoop loop;
    private TrayHost? tray;
    private ProgramInfoWindow? program;
    private ProgramInfoTarget? programTarget;
    private NotificationEvent? lastClick;
    private int clickedMember=-1;
    private DispatcherTimer? maintenance;
    private UiSnapshot? latest;
    private int posted;
    private bool stopping,finished,stale;
    private bool? pendingPause;
    private (long Session,string Key,string Program,string Path)? requestedRecent;

    public WindowController(MainWindow window,Options options)
    {
        this.window=window; this.options=options; loop=new(options);
        window.CommandRequested+=Act;
        window.ProcessSelectionChanged+=row=> {
            if(lastClick is { } copy && clickedMember>=0 && (!UiIdentity.IsVerified(window.CurrentSnapshot,row) ||
                row!.Key!=copy.Members[clickedMember].Key.ToString() || row.ProgramId!=copy.ProgramId.ToString())) {
                clickedMember=-1;
                // 다음 worker frame 전에도 선택 해제한 옛 사건이 재선택 때 되살아나지 않는다.
                window.RenderSnapshot(WithClickedEvidence(window.CurrentSnapshot));
            }
            if(program!=null && programTarget?.SameInstance(Target())!=true) program.Close();
            SyncRecentSelection();
        };
        window.SourceInitialized+=(_,_)=> {
            tray=new TrayHost(window);
            tray.ActionReceived+=OnTray;
            maintenance=new DispatcherTimer(TimeSpan.FromSeconds(1),DispatcherPriority.Background,(_,_)=>Tick(),window.Dispatcher);
            loop.Start();
        };
        window.Closing+=Closing;
        loop.Published+=Publish;
        loop.Status+=s=>OnUi(()=>window.SetCommandStatus(s));
        loop.GroupStatus+=(session,s)=>OnUi(()=>window.SetGroupCommandStatus(session,s));
        loop.Navigate+=(ev,member,metric)=>OnUi(()=>Navigate(ev,member,metric));
        loop.RequestShell=value=>window.Dispatcher.Invoke(()=>!stopping && tray!=null ? tray.Request(value) : TrayRequestResult.Unavailable);
        loop.CancelShell=id=>window.Dispatcher.Invoke(()=>tray?.Cancel(id));
        loop.Finished+=exit=>window.Dispatcher.BeginInvoke(new Action(()=> {
            finished=true; maintenance?.Stop(); loop.ReleaseWakeHandle(); tray?.Dispose(); program?.Close();
            window.Close(); Application.Current.Shutdown(exit);
        }));
    }
    private void OnUi(Action action) => window.Dispatcher.BeginInvoke(new Action(()=> { if(!stopping) action(); }));
    private void Publish(UiSnapshot frame)
    {
        // UI가 늦으면 중간 frame은 건너뛰고 최신 복사본 하나만 넘긴다. native 포인터는 없다.
        Interlocked.Exchange(ref latest,frame);
        if(Interlocked.Exchange(ref posted,1)==0) OnUi(Consume);
    }
    private void Consume()
    {
        Interlocked.Exchange(ref posted,0);
        if(Interlocked.Exchange(ref latest,null) is not { } frame) return;
        if(window.CurrentSnapshot.SessionEpoch!=frame.SessionEpoch) {lastClick=null; clickedMember=-1;}
        if(frame.Paused==pendingPause || frame.SessionEpoch<0) pendingPause=null;
        stale=false;
        if(tray!=null) tray.Enabled=frame.NotificationsEnabled;
        window.RenderSnapshot(WithClickedEvidence(frame));
        SyncRecentSelection();
    }
    private void SyncRecentSelection()
    {
        var snapshot=window.CurrentSnapshot;
        var row=window.SelectedRow;
        (long Session,string Key,string Program,string Path)? target=UiIdentity.IsVerified(snapshot,row) && row!.Path.Length!=0
            ? (snapshot.SessionEpoch,row.Key,row.ProgramId,row.Path) : null;
        if(target==requestedRecent) return;
        requestedRecent=target;
        loop.SelectRecent(snapshot,row);
    }
    private UiSnapshot WithClickedEvidence(UiSnapshot frame)
    {
        // 입력이 이미 화면에 적용한 복사본이어도 이전 클릭만 벗긴다. 수락 원본·현재 표본·
        // 장기 보고서는 그대로 두므로 연속 클릭이나 선택 해제가 옛 사건을 누적하지 않는다.
        if(frame.Rows.Any(r=>r.ClickedAlerts.Count!=0))
            frame=frame with {Rows=frame.Rows.Select(r=>r.ClickedAlerts.Count==0 ? r : r with {ClickedAlerts=[]}).ToArray()};
        if(clickedMember<0 || lastClick is not { } copy) return frame;
        if(frame.SessionEpoch!=copy.Session) {clickedMember=-1;return frame;}
        var member=copy.Members[clickedMember];
        var key=member.Key.ToString();
        var target=UiIdentity.FindVerified(frame,key);
        var index=-1;
        if(target!=null && target.ProgramId==copy.ProgramId.ToString())
            for(int i=0;i<frame.Rows.Count;++i) if(ReferenceEquals(frame.Rows[i],target)) {index=i;break;}
        if(index<0) {clickedMember=-1;return frame;}
        var rows=frame.Rows.ToArray();
        var saved=Enumerable.Range(0,4).Where(m=>(member.Metrics & (1u<<m))!=0)
            .Select(m=>Projection.Alert(member,m,null)).ToArray();
        rows[index]=rows[index] with {ClickedAlerts=saved};
        return frame with {Rows=rows};
    }
    private void Tick()
    {
        if(stopping) return;
        if(tray?.Tick() is ulong retired && retired!=0) loop.Tray(TrayAction.Retire,retired);
        var s=window.CurrentSnapshot;
        if(!stale && options.Mode==InputMode.Live && !s.Paused && s.ObservedAt.HasValue && DateTimeOffset.UtcNow-s.ObservedAt.Value>TimeSpan.FromSeconds(15)) {
            stale=true;
            window.RenderSnapshot(s with {Status="마지막 관측이 오래됨"});
        }
    }
    // UI를 먼저 닫지 않고 worker에 중단을 요청한다. 현재 회차와 native 자원 정리가
    // 끝난 Finished 콜백이 tray·프로그램 창·wake를 정리한 뒤 실제 종료를 수행한다.
    private void Closing(object? sender,CancelEventArgs e)
    {
        if(finished) return;
        e.Cancel=true;
        if(stopping) return;
        stopping=true; maintenance?.Stop(); program?.CancelPending();
        window.SetCommandStatus("현재 회차를 정리하고 있습니다."); loop.Stop();
    }
    private void Restore()
    {
        window.Show(); if(window.WindowState==WindowState.Minimized) window.WindowState=WindowState.Normal;
        window.Activate();
    }
    private void OnTray(TrayAction action,ulong id)
    {
        if(stopping) return;
        switch(action) {
            case TrayAction.ShowWindow: Restore(); break;
            case TrayAction.Toggle: Act(new UiCommand("toggle-notifications")); break;
            case TrayAction.Exit: window.Close(); break;
            case TrayAction.Click: Restore(); loop.Tray(action,id); break;
            case TrayAction.Retire: loop.Tray(action,id); break;
        }
    }
    private ProgramInfoTarget? Target()
    {
        var s=window.CurrentSnapshot; var r=window.SelectedRow;
        if(stopping || pendingPause.HasValue || s.Paused || !UiIdentity.IsVerified(s,r) || (!s.IsSynthetic && r!.Path.Length==0)) return null;
        if(r==null) return null;
        if(!s.IsSynthetic && (r.ObservedAt is not { } observed || DateTimeOffset.UtcNow<observed || DateTimeOffset.UtcNow-observed>TimeSpan.FromSeconds(15))) return null;
        return new((uint)s.SessionEpoch,r.Pid,r.Creation,r.Path,r.Name,s.IsSynthetic);
    }
    private void Act(UiCommand command)
    {
        if(stopping) return;
        var s=window.CurrentSnapshot;
        switch(command.Name) {
            case "program-info":
                if(Target() is not { } target) {window.SetCommandStatus("새 표본에서 실행 경로가 확인된 구성원을 선택하세요.");return;}
                program?.Close();
                var info=new ProgramInfoWindow(window,target,Target); program=info; programTarget=target;
                info.Closed+=(_,_)=> {if(ReferenceEquals(program,info)) {program=null;programTarget=null;}};
                info.Show(); return;
            case "notification-members": ShowMembers(); return;
            case "admin-relaunch": Admin(); return;
        }
        if(command.Name is "pause" or "resume") { pendingPause=command.Name=="pause"; program?.Close(); }
        loop.Act(command,s,window.SelectedRow);
    }
    private void Admin()
    {
        using var identity=System.Security.Principal.WindowsIdentity.GetCurrent();
        if(new System.Security.Principal.WindowsPrincipal(identity).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator)) {
            window.SetCommandStatus("관리자 권한으로 실행 중입니다."); return;
        }
        // 현재 창을 닫기 전에 승격을 요청해 취소 시 이 수집을 유지한다.
        // 새 인스턴스는 이 인스턴스의 종료를 확인한 뒤 수집을 시작한다.
        var start=new ProcessStartInfo(Environment.ProcessPath!) {UseShellExecute=true,Verb="runas"};
        string[] args=Environment.GetCommandLineArgs();
        for(int i=1;i<args.Length;i++) {
            if(args[i]=="--wait-for-instance") {i+=2; continue;}
            if(args[i]=="--diagnostics") {i++; continue;}
            start.ArgumentList.Add(args[i]);
        }
        if(options.Mode==InputMode.Live && !args.Contains("--db")) { start.ArgumentList.Add("--db"); start.ArgumentList.Add(options.Database); }
        using var current=Process.GetCurrentProcess();
        start.ArgumentList.Add("--wait-for-instance"); start.ArgumentList.Add(current.Id.ToString());
        start.ArgumentList.Add(current.StartTime.ToUniversalTime().ToFileTimeUtc().ToString());
        try {
            using var child=Process.Start(start);
            if(child!=null) window.Close();
        } catch(Win32Exception ex) {window.SetCommandStatus(ex.NativeErrorCode==1223?"관리자 실행 요청이 취소되었습니다.":ex.Message);}
    }
    private void Navigate(NotificationEvent ev,int index,int metric)
    {
        // 대기 frame부터 소비한 뒤 세션과 worker의 현재 신원 검증을 확인한다.
        // 대상이 없어도 lastClick은 남겨 당시 복사 근거를 메뉴에서 읽을 수 있게 한다.
        Consume();
        if(ev.Session!=window.CurrentSnapshot.SessionEpoch || window.CurrentSnapshot.Paused || pendingPause.HasValue) {
            window.SetCommandStatus("이 알림의 관측 세션이 끝났습니다."); return;
        }
        lastClick=ev; clickedMember=-1;
        var m=ev.Members[index];
        var target=UiIdentity.FindVerified(window.CurrentSnapshot,m.Key.ToString());
        if(m.RowIndex<0 || target==null || target.ProgramId!=ev.ProgramId.ToString()) {
            window.SetCommandStatus("현재 인스턴스를 확인할 수 없습니다. 메뉴에서 알림 당시 근거를 확인할 수 있습니다.");return;
        }
        clickedMember=index;
        window.RenderSnapshot(WithClickedEvidence(window.CurrentSnapshot));
        if(window.SelectProcess(target.Key,(UiMetric)metric)) {
            window.SetSidebarVisible(true); Restore();
            window.SetCommandStatus($"알림 대상 · {target.Name} · {ViewFormat.Metric((UiMetric)metric)}");
        }
    }
    private void ShowMembers()
    {
        if(lastClick is not { } ev) { window.SetCommandStatus("이번 세션에서 연 Windows 알림이 없습니다."); return; }
        var panel=new StackPanel { Margin=new Thickness(20) };
        var dialog=new Window {Title="알림 당시 근거",Owner=window,Width=560,MaxHeight=580,SizeToContent=SizeToContent.Height,
            WindowStartupLocation=WindowStartupLocation.CenterOwner,Content=new ScrollViewer {Content=panel,VerticalScrollBarVisibility=ScrollBarVisibility.Auto}};
        panel.Children.Add(new TextBlock {Text=ev.Name,FontSize=20,Margin=new Thickness(0,0,0,12)});
        for(int i=0;i<ev.Members.Length;i++) {
            var member=ev.Members[i];
            foreach(int metric in Enumerable.Range(0,4).Where(m=>(member.Metrics & (1u<<m))!=0)) {
                var cause=(UiMetric)metric; var e=member.Metric[metric]; int memberIndex=i;
                var button=new Button { Content=$"PID {member.Key.Pid} · {ViewFormat.Metric(cause)} · 현재 대상 확인",Margin=new Thickness(0,4,0,2),Padding=new Thickness(5) };
                button.Click+=(_,_)=> { dialog.Close(); loop.ChooseMember(ev.Id,memberIndex,metric); }; panel.Children.Add(button);
                panel.Children.Add(new TextBlock {Text=$"{ViewFormat.Window(Projection.Time(e.Start),Projection.Time(e.End))}\n당시 평균 {ViewFormat.Value(cause,e.Current)} · 당시 상한 {ViewFormat.Value(cause,e.Threshold)} · 연속 {e.Streak}회",TextWrapping=TextWrapping.Wrap});
            }
        }
        if(ev.OmittedCount>0) panel.Children.Add(new TextBlock {Text=$"추가 후보 {ev.OmittedCount}개의 개별 근거는 포함되지 않았습니다."});
        dialog.Show();
    }
}
