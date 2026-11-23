using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;

namespace PcMonitor;

// Engine·최근 조회·알림 근거를 한 worker에서 소유한다. Run의 명령 → 수집 → 알림 → 대기
// 순서가 실행 시작점이다. UI는 복사된 UiSnapshot만 받고, 선택/명령은 Enqueue로 되돌린다.
internal sealed class MonitorLoop
{
    private readonly Options options;
    private readonly Thread thread;
    private readonly ConcurrentQueue<Action> commands=new();
    private readonly AutoResetEvent wake=new(false);
    private readonly Projection projection=new();
    // 수락한 근거는 인스턴스+program별로 지표마다 보관한다. 새 같은 지표 알림은 교체하고
    // 현재 회차에서 사라진 대상은 제거한다. 실패·재개 때 전체를 비워 다른 세션에 잇지 않는다.
    private readonly Dictionary<(ProcessKey,long),UiAlert[]> evidence=new();
    private Engine? engine;
    private SystemUsage? system;
    private SystemReading systemFrame=new();
    private Snapshot? frame;
    private NotificationEvent? clicked, activeShell;
    private StreamWriter? diagnostics;
    private volatile bool stopping;
    private bool paused, enabled=true, failed;
    private long nextSample, nextPump, cycles;
    private uint demoSession=1;
    private sealed record RecentTarget(uint Session, ProcessKey Process, long Program, string Path);
    private RecentTarget? recentTarget;
    private UiRecentComparison[] recentComparisons=[];
    private ulong recentSequence=ulong.MaxValue;
    private long recentRequests;

    public event Action<UiSnapshot>? Published;
    public event Action<string>? Status;
    // 빈 그룹 결과는 직전 그룹 결과만 대체한다. 일반 Status를 비우는 요청이 아니다.
    public event Action<uint,string>? GroupStatus;
    public event Action<NotificationEvent,int,int>? Navigate;
    public event Action<int>? Finished;
    public Func<NativeNotification,TrayRequestResult>? RequestShell;
    public Action<ulong>? CancelShell;

    public MonitorLoop(Options options)
    {
        this.options=options;
        thread=new Thread(Run) { Name="PC resource monitor worker", IsBackground=true };
    }
    public void Start() => thread.Start();
    private void Enqueue(Action command)
    {
        if(stopping) return;
        commands.Enqueue(command);
        wake.Set();
    }
    public void Stop() { stopping=true; wake.Set(); }
    // worker의 정리가 끝난 Finished를 UI가 소비한 뒤에만 대기 핸들을 해제한다.
    public void ReleaseWakeHandle() => wake.Dispose();
    private void Record(object item) { if(diagnostics!=null) diagnostics.WriteLine(JsonSerializer.Serialize(item)); }
    // WindowController가 시작한 전용 worker다. 명령을 먼저 적용한 뒤 수집과 알림 요청을
    // 순서대로 처리한다. 일반 명령 오류는 상태만 알리지만 Engine이나 수집이 실패하면
    // Fail로 표본·근거를 비우고 다음 수집을 멈춘다. 종료 때 native 자원을 해제한 뒤
    // Finished를 보내며, UI는 그 통지를 받은 뒤 wake 핸들을 해제한다.
    private void Run()
    {
        int exit=0;
        try {
            try {
                options.WaitForPrevious();
                if(options.Diagnostics!=null) {
                    diagnostics=new StreamWriter(new FileStream(options.Diagnostics,FileMode.CreateNew,FileAccess.Write,FileShare.Read)) {AutoFlush=true};
                }
                if(options.Mode is InputMode.Live or InputMode.PersistedSynthetic) {
                    Directory.CreateDirectory(Path.GetDirectoryName(options.Database)!);
                    engine=Engine.Open(options.Database,options.Mode==InputMode.PersistedSynthetic);
                    if(options.Mode==InputMode.Live) system=new SystemUsage();
                } else if(options.Mode==InputMode.NotificationDemo) engine=Engine.OpenNotificationDemo();
                engine?.SetNotifications(enabled);
            } catch(Exception e) { Fail(e); exit=1; }
            while(!stopping) {
                while(!stopping && commands.TryDequeue(out var command)) {
                    try { command(); }
                    catch(Exception e) when (engine?.Failed == true) { Fail(e); exit=1; }
                    catch(Exception e) { Status?.Invoke(e.Message); Record(new { action="command-error",error=e.Message }); }
                }
                if(stopping) break;
                long now=Environment.TickCount64;
                if(!failed && !paused && now>=nextSample) {
                    try {
                        Sample();
                        if(options.Count>0 && cycles>=options.Count) break;
                    } catch(Exception e) { Fail(e); exit=1; }
                    nextSample=Environment.TickCount64+options.Interval;
                }
                if(failed && options.Count>0) break;
                if(!failed && !paused && Environment.TickCount64>=nextPump) {
                    Pump();
                    nextPump=Environment.TickCount64+500;
                }
                int wait=paused||failed ? Timeout.Infinite :
                    (int)Math.Clamp(Math.Min(nextSample,nextPump)-Environment.TickCount64,0,60000);
                if(!stopping) wake.WaitOne(wait);
            }
        } catch(Exception e) { Fail(e); exit=1; }
        finally {
            stopping=true;
            system?.Dispose(); engine?.Dispose();
            diagnostics?.Dispose();
            Finished?.Invoke(exit);
        }
    }
    private void Fail(Exception e)
    {
        failed=true;
        CancelActive();
        frame=null; evidence.Clear(); clicked=null; ClearRecent();
        Published?.Invoke(new UiSnapshot {SessionEpoch=-1,Status="수집 중단",CommandStatus=e.Message,Footer=e.Message,NotificationsEnabled=enabled});
        Status?.Invoke(e.Message);
        Record(new {action="failed",error=e.Message});
    }
    private void Sample()
    {
        // GPU 구간을 먼저 닫고 native collector가 행별 현재값·시각을 수집한다.
        // 전체·그룹·개별 표시에는 같은 발행 회차의 값을 함께 전달한다.
        if(system!=null) systemFrame=system.Read();
        if(engine!=null) frame=engine.Collect();
        RefreshRecent();
        ++cycles;
        if(evidence.Count>0 && frame!=null) {
            var live=frame.Rows.Select(r=>(r.Key,r.ProgramId)).ToHashSet();
            foreach(var key in evidence.Keys.Where(k=>!live.Contains(k)).ToArray()) evidence.Remove(key);
        }
        Publish();
    }
    private void Publish()
    {
        UiSnapshot ui;
        if(options.Mode==InputMode.ScreenDemo) ui=Demo.Frame(demoSession,cycles,paused,enabled);
        else if(options.Mode==InputMode.SmallGroupsDemo) ui=Demo.SmallGroupsFrame(demoSession,cycles,paused,enabled);
        else if(frame!=null) ui=projection.Build(frame with {Paused=paused},systemFrame,enabled,evidence);
        else ui=new() {SessionEpoch=demoSession,Status="첫 표본 대기",IsSynthetic=options.Mode!=InputMode.Live,NotificationsEnabled=enabled};
        ui=AttachRecentComparisons(ui);
        Published?.Invoke(ui);
        if(diagnostics!=null) Record(new {action="frame",session=ui.SessionEpoch,sequence=ui.Sequence,paused,rows=ui.Rows.Count,groups=ui.Groups.Count,
            utc=frame?.MeasuredUtc,windows=frame?.CompletedWindows,cpu=ui.Cpu.Total,gpu=ui.Gpu.Total,ram=ui.Ram.Total,
            gpuRows=ui.Rows.Count(r=>r.GpuPercent.HasValue),privateRamRows=frame?.Rows.Count(r=>r.PrivateWorkingSet.HasValue),
            collectMs=frame?.CollectMs,storeMs=frame?.StoreMs,groupMs=frame?.GroupingMs,recentRequests,
            syntheticRow=options.Mode==InputMode.PersistedSynthetic && ui.Rows.Count>0?ui.Rows[0]:null});
    }
    // 이미 조회한 근거를 동일 세션·인스턴스·실행 경로의 선택 행에만 붙인다.
    // Publish는 DB를 읽지 않는다. 검색·정렬·목록 갱신이 SQL 조회를 늘리지 않는 경계다.
    private UiSnapshot AttachRecentComparisons(UiSnapshot ui)
    {
        if(recentTarget is not { } target || ui.SessionEpoch!=target.Session || recentComparisons.Length==0) return ui;
        var rows=ui.Rows.ToArray();
        var selected=UiIdentity.FindVerified(ui,target.Process.ToString());
        if(selected==null || selected.ProgramId!=target.Program.ToString(System.Globalization.CultureInfo.InvariantCulture) || selected.Path!=target.Path) return ui;
        int i=Array.FindIndex(rows,r=>ReferenceEquals(r,selected));
        if(i<0) return ui;
        rows[i]=rows[i] with {RecentComparisons=recentComparisons};
        return ui with {Rows=rows};
    }
    private void ClearRecent()
    {
        recentTarget=null; recentComparisons=[]; recentSequence=ulong.MaxValue;
    }
    // 선택이 바뀌면 이전 숫자와 조회 회차를 함께 비운다. UI가 본 세션과 worker 세션이
    // 다르거나 그룹/선택 해제/신원 불명인 경우에는 조회 대상을 만들지 않는다.
    public void SelectRecent(UiSnapshot shown, UiRow? selected) => Enqueue(()=> {
        RecentTarget? target=UiIdentity.IsVerified(shown,selected) && selected!=null &&
            selected.Path.Length!=0 && long.TryParse(selected.ProgramId,out long program) && program>0 &&
            frame is { } current && shown.SessionEpoch==current.Session
            ? new(current.Session,new(selected.Pid,selected.Creation),program,selected.Path) : null;
        if(target==recentTarget) return;
        recentTarget=target; recentComparisons=[]; recentSequence=ulong.MaxValue;
        RefreshRecent(); Publish();
    });
    private static ProcessRow? FindRecentRow(Snapshot current,RecentTarget target)
    {
        var matching=current.Rows.Where(r=>r.Key.Pid==target.Process.Pid).ToArray();
        return current.Session==target.Session && matching.Length==1 && matching[0].Key==target.Process && matching[0].ProgramId==target.Program &&
            matching[0].Path==target.Path && matching[0].Valid.HasFlag(ResourceValid.Identity) ? matching[0] : null;
    }
    // 선택 변경·새 수집 회차·성공한 피드백 뒤 호출한다. 같은 회차의 단순 재발행·실패 재시도는
    // 다시 조회하지 않는다. 비치명적 조회 실패는 근거를 비우고 수집 자체는 계속한다.
    private void RefreshRecent()
    {
        if(engine==null || recentTarget is not { } target || frame is not { } current) return;
        if(FindRecentRow(current,target) is not { } row) {ClearRecent();return;}
        if(recentSequence==current.Sequence) return;
        recentSequence=current.Sequence;
        // 네 지표 모두 장기 기준이 준비된 경우에만 조회를 생략한다. 일부 지표만 준비됐으면
        // 최근 근거도 보존하고 MainWindow가 지표별로 알림 → 장기 → 최근 우선순위를 고른다.
        if(row.Metric.Length==4 && row.Metric.All(m=>m.State>=MetricState.Normal)) {recentComparisons=[];return;}
        try {
            ++recentRequests;
            if(!engine.TryGetRecentComparison(current.Session,current.Sequence,target.Process,target.Program,target.Path,out var reports)) {
                // 선택 이후 대상이 바뀐 결과는 DB 장애가 아니다. 다음 선택/회차에서 다시
                // 대조할 수 있게 숫자만 비우고, 상태 경고나 수집 중단을 만들지 않는다.
                recentComparisons=[];
                Record(new {action="recent-comparison-target-changed"});
                return;
            }
            recentComparisons=reports.Select(Projection.Recent).ToArray();
        } catch(EngineException e) when(!engine.Failed) {
            recentComparisons=[];
            Status?.Invoke("최근 기록 비교를 확인할 수 없습니다. · "+e.Message);
            Record(new {action="recent-comparison-error",error=e.Message});
        }
    }
    private void CancelActive()
    {
        if(activeShell!=null) { CancelShell?.Invoke(activeShell.Id); activeShell=null; }
    }
    // Shell이 요청을 수락한 지표만 당시 근거로 보관한다. 최신 표본으로 바꾸지 않고,
    // 같은 인스턴스·프로그램의 다른 지표 근거는 그대로 남긴다.
    private void RememberAcceptedEvidence(NotificationEvent ev,DateTimeOffset accepted)
    {
        foreach(var member in ev.Members) {
            var key=(member.Key,ev.ProgramId);
            var copies=(evidence.GetValueOrDefault(key)??[]).ToDictionary(a=>(int)a.Metric);
            for(int m=0;m<4;++m) if((member.Metrics & (1u<<m))!=0) copies[m]=Projection.Alert(member,m,accepted);
            evidence[key]=copies.Values.OrderByDescending(a=>a.AcceptedAt).ThenBy(a=>a.Metric).ToArray();
        }
    }
    // 정책의 begin/dispatch 뒤 UI Dispatcher에서 Shell 접수를 요청하고 결과를 반드시
    // 한 번 보고한다. Requested일 때만 당시 근거를 보관하며 실제 사용자 열람 여부와는 다르다.
    private void Pump()
    {
        if(engine==null || stopping || !enabled || engine.BeginNotification() is not { } claim) return;
        if(engine.DispatchNotification(claim.Id) is not { } ev) return;
        var result=TrayRequestResult.Unavailable;
        try { if(!stopping && RequestShell!=null) result=RequestShell(ev.Native); }
        finally { engine.ReportNotification(ev,result==TrayRequestResult.Requested); }
        Record(new {action="shell-request",id=ev.Id,result=result.ToString()});
        if(result!=TrayRequestResult.Requested) { Status?.Invoke($"Windows 알림 전달 상태 · {result}"); return; }
        activeShell=ev;
        RememberAcceptedEvidence(ev,DateTimeOffset.UtcNow);
        Publish();
    }
    public void Tray(TrayAction action,ulong id) => Enqueue(()=> {
        if(engine==null) return;
        if(action==TrayAction.Retire) {engine.RetireNotification(id); if(activeShell?.Id==id) activeShell=null; return;}
        if(action!=TrayAction.Click) return;
        var ev=engine.ClickNotification(id);
        Record(new {action="notification-click",id,validated=ev?.Members.FirstOrDefault()?.RowIndex>=0});
        if(ev==null) {Status?.Invoke("알림이 만료되었거나 관측 세션이 바뀌었습니다."); return;}
        clicked=ev; activeShell=null;
        Navigate?.Invoke(ev,0,ev.Cause);
    });
    public void ChooseMember(ulong id,int member,int metric) => Enqueue(()=> {
        var ev=clicked;
        if(engine==null || ev==null || ev.Id!=id || member<0 || member>=ev.Members.Length ||
            metric<0 || metric>=4 || (ev.Members[member].Metrics & (1u<<metric))==0) {Status?.Invoke("이 알림의 복사 근거를 다시 확인하세요."); return;}
        var members=(NotificationMember[])ev.Members.Clone();
        int index=engine.ValidateMember(ev,member);
        members[member]=members[member] with {RowIndex=index,ValidationError=index<0?"현재 대상 신원을 확인할 수 없습니다.":""};
        Navigate?.Invoke(ev with {Members=members},member,metric);
    });
    // UI가 보던 선택을 명령에 담아 worker로 넘긴다. 실행 시 세션과 대상/구성원을 다시
    // 확인하고, 피드백·소속 변경 성공 뒤 반환된 복사본을 다시 발행한다.
    public void Act(UiCommand command,UiSnapshot shown,UiRow? selected) => Enqueue(()=> {
        if(failed) return;
        uint session=frame?.Session??demoSession;
        if(shown.SessionEpoch!=session) { Status?.Invoke("관측 세션이 바뀌었습니다. 현재 항목을 다시 선택하세요."); return; }
        switch(command.Name) {
            case "pause":
                if(paused) return;
                engine?.Pause(); paused=true; system?.Dispose(); system=null; CancelActive(); Publish(); return;
            case "resume":
                if(!paused) return;
                engine?.Resume(); paused=false; frame=null; clicked=null; evidence.Clear(); ClearRecent(); ++demoSession;
                if(options.Mode==InputMode.Live) system=new SystemUsage();
                systemFrame=new(); nextSample=0; nextPump=0; return;
            case "toggle-notifications":
                enabled=!enabled; engine?.SetNotifications(enabled); if(!enabled) CancelActive(); Publish(); return;
            case "demo-notification":
                if(engine?.IsNotificationDemo!=true || paused || !enabled) {Status?.Invoke("--ui-notification-demo 수집 중에 검수 알림을 보낼 수 있습니다.");return;}
                frame=engine.InjectNotificationDemo(); Publish(); Status?.Invoke("합성 후보를 알림 정책에 전달했습니다."); return;
        }
        if(engine==null || frame==null) { Status?.Invoke("화면 예시에서는 저장 및 그룹 지정을 사용할 수 없습니다."); return; }
        // 큐 대기 중 새 표본이 왔거나 표시 입력 자체가 모호할 수 있다. 선택 행과 실제
        // worker 복사본 양쪽에서 PID·생성시각·경로를 확인한 뒤에만 native 명령을 보낸다.
        if(selected!=null || command.ProcessKey!=null) {
            bool valid=UiIdentity.IsVerified(shown,selected) && selected!=null &&
                (command.ProcessKey==null || command.ProcessKey==selected.Key) &&
                long.TryParse(selected.ProgramId,out var selectedProgram) &&
                FindRecentRow(frame,new(session,new(selected.Pid,selected.Creation),selectedProgram,selected.Path))!=null;
            if(!valid) {
                const string reason="현재 신원이 유일하게 확인된 구성원을 다시 선택하세요.";
                if(command.Name.StartsWith("group-",StringComparison.Ordinal)) GroupStatus?.Invoke(session,reason); else Status?.Invoke(reason);
                return;
            }
        }
        if(selected==null && (command.Name.StartsWith("feedback-",StringComparison.Ordinal) || command.Name is "group-move" or "group-detach" or "group-auto")) {
            Status?.Invoke("현재 신원이 유일하게 확인된 구성원을 다시 선택하세요."); return;
        }
        var key=selected==null ? default : new ProcessKey(selected.Pid,selected.Creation);
        if(command.Name.StartsWith("feedback-",StringComparison.Ordinal)) {
            if(selected==null || !long.TryParse(selected.ProgramId,out var program) || program<=0) {Status?.Invoke("실행 경로가 확인된 구성원을 선택하세요.");return;}
            var kind=command.Name switch {"feedback-suppress-hour"=>FeedbackKind.Ignore,"feedback-expected-day"=>FeedbackKind.Expected,"feedback-clear"=>FeedbackKind.Cancel,_=>throw new ArgumentException(command.Name)};
            frame=engine.Feedback(session,key,program,kind);
            if(kind!=FeedbackKind.Cancel && activeShell?.ProgramId==program) CancelActive();
            // 피드백 뒤 별도 보관 정리가 과거 창을 지워도 sequence는 그대로다. 현재 선택만
            // 다시 확인하며 native의 유효 캐시는 유지해 삭제 없는 경우 추가 SQL을 피한다.
            recentSequence=ulong.MaxValue; RefreshRecent();
            Publish(); Status?.Invoke("이 프로그램 경로의 알림 설정을 저장했습니다."); return;
        }
        var nativeGroup=frame.Groups.FirstOrDefault(g=>Projection.Id(g.Key)==command.GroupKey);
        GroupAction edit=command.Name switch {
            "group-confirm" => GroupAction.Confirm, "group-target" => selected==null?GroupAction.GroupTarget:GroupAction.ProcessTarget,
            "group-move" => GroupAction.Move, "group-detach" => GroupAction.SeparateShared,
            "group-auto" => GroupAction.RestoreAuto, _=>throw new ArgumentException(command.Name)
        };
        if(edit is GroupAction.Confirm or GroupAction.GroupTarget) {
            if(nativeGroup==null) {GroupStatus?.Invoke(session,"소속 그룹이나 개별 구성원을 선택하세요.");return;}
            var shownMembers=shown.Rows.Where(r=>r.GroupKey==command.GroupKey).Select(r=>new ProcessKey(r.Pid,r.Creation)).ToHashSet();
            if(shownMembers.Count!=nativeGroup.Members.Length || !shownMembers.SetEquals(nativeGroup.Members.Select(i=>frame.Rows[i].Key))) {
                GroupStatus?.Invoke(session,"그룹 구성원이 바뀌었습니다. 새 목록에서 다시 지정하세요."); return;
            }
        }
        try { frame=engine.Edit(session,edit,key,nativeGroup?.Key??default); }
        catch(EngineException e) when (!engine.Failed) {
            GroupStatus?.Invoke(session,e.Message); Record(new {action="command-error",error=e.Message}); return;
        }
        Publish();
        // 부분 성공도 편집 수락이다. 재적용하지 않으며 대기는 복사본에 남긴다.
        // 다음 정상 목록은 대기 비트만 해제하므로 이후의 다른 명령 안내를 지우지 않는다.
        GroupStatus?.Invoke(session,frame.GroupRefreshPending ? "" :
            "이번 세션의 그룹 지정을 적용했습니다."+(frame.TargetLabel.Length>0?$" 이동 대상 · {frame.TargetLabel}":""));
    });
}
