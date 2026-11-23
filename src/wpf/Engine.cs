// MonitorLoop가 사용하는 C DLL 어댑터와 관리 자료형이다. Engine.Create에서 ABI를
// 확인하고 CopyRequired에서 native의 빌린 배열·문자열을 관리 복사본으로 바꾼다.
using System;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace PcMonitor;

public readonly record struct ProcessKey(uint Pid, ulong Creation)
{
    public bool IsKnown => Pid != 0 && Creation != 0;
    public override string ToString() => $"{Pid}:{Creation}";
}
public readonly record struct GroupKey(int Kind, ProcessKey Anchor, ulong Id);
public enum MetricState { Unavailable, Learning, Normal, Attention, Candidate }
public enum FeedbackKind { Cancel, Ignore, Expected }
public enum GroupAction { Confirm, GroupTarget, ProcessTarget, Move, SeparateShared, RestoreAuto, Expand, Collapse, ExpandAll, CollapseAll }
[Flags] public enum ResourceValid : uint
{
    Name=1, Identity=2, Cpu=4, Memory=8, Read=16, Write=32, Elapsed=64, Foreground=128, ObservationInterval=256
}
// 같은 인스턴스의 마지막 완료창 값 사본이다. 현재 표본이나 현재 연속 판정과 수명이
// 다르며 새 완료창/신원·세션 변경/UTC 역행에서 교체한다. NaN은 0과 구분한다.
public readonly record struct CompletedMetricEvidence(MetricState State,double Average,double Median,double Upper,ulong Start,ulong End,double Duration);
public sealed record MetricReport
{
    public MetricState State { get; init; }
    // 완료 근거는 표시 전용이다. 현재 결측/긴 간격이 State를 초기화해도 같은 출처의
    // Completed를 유지하지만 후보 정책은 State를 사용한다. null은 완료 근거 없음이다.
    public CompletedMetricEvidence? Completed { get; init; }
    public MetricState? CompletedState => Completed?.State;
    // Current는 마지막 완료 구간의 평균이다. 현재 원시 표본은 ProcessRow의 값을 읽는다.
    public double Current { get; init; }
    public double Median { get; init; }
    public double Mad { get; init; }
    public double Threshold { get; init; }
    public double CoveredSeconds { get; init; }
    public uint Count { get; init; }
    public uint Days { get; init; }
    public uint Minutes { get; init; }
    public uint Streak { get; init; }
    public uint ValidSamples { get; init; }
    public ulong Start { get; init; }
    public ulong End { get; init; }
    public double Duration { get; init; }
    public bool BaselineEligible { get; init; }
    public string Reason { get; init; } = "";
}
public sealed record RecentMetricReport
{
    public uint State { get; init; }
    public uint Count { get; init; }
    public long AnchorWindowId { get; init; }
    public double Current { get; init; }
    public double Median { get; init; }
    public double Duration { get; init; }
    public ulong Start { get; init; }
    public ulong End { get; init; }
    public ulong HistoryStart { get; init; }
    public ulong HistoryEnd { get; init; }
}
public sealed record ProcessRow
{
    public ProcessKey Key { get; init; }
    public string Name { get; init; } = "";
    public string Path { get; init; } = "";
    public string SnapshotName { get; init; } = "";
    public string DisplayName => Valid.HasFlag(ResourceValid.Name) && Name.Length != 0
        ? Name : NameFromList && SnapshotName.Length != 0 ? SnapshotName : $"PID {Key.Pid}";
    public bool NameFromList { get; init; }
    public ResourceValid Valid { get; init; }
    public ulong MeasuredUtc { get; init; }
    public long ProgramId { get; init; }
    public double Cpu { get; init; }
    public ulong Memory { get; init; }
    public ulong? PrivateWorkingSet { get; init; }
    public double Read { get; init; }
    public double Write { get; init; }
    public double Elapsed { get; init; }
    public double ObservationInterval { get; init; }
    public double CpuInterval { get; init; }
    public double IoInterval { get; init; }
    public int Foreground { get; init; }
    public int CpuStatus { get; init; }
    public int ReadStatus { get; init; }
    public int WriteStatus { get; init; }
    public uint OpenError { get; init; }
    public uint TimesError { get; init; }
    public uint MemoryError { get; init; }
    public uint IoError { get; init; }
    public uint NameError { get; init; }
    public uint PathError { get; init; }
    public uint QueryAttempts { get; init; }
    public uint ExitError { get; init; }
    public uint ExitCode { get; init; }
    public bool ExitObserved { get; init; }
    public int SpecialProcess { get; init; }
    public int RelationGrade { get; init; }
    public int RelationCut { get; init; }
    public MetricReport[] Metric { get; init; } = Array.Empty<MetricReport>();
    public FeedbackKind Feedback { get; init; }
    public ulong FeedbackExpires { get; init; }
    public double? Value(int metric) => metric switch
    {
        0 when Valid.HasFlag(ResourceValid.Cpu) => Cpu,
        1 when Valid.HasFlag(ResourceValid.Memory) => Memory,
        2 when Valid.HasFlag(ResourceValid.Read) => Read,
        3 when Valid.HasFlag(ResourceValid.Write) => Write,
        _ => null
    };
}
public sealed record GroupSum
{
    public double Value { get; init; }
    public ulong Bytes { get; init; }
    public int Valid { get; init; }
    public int Total { get; init; }
    public bool Overflow { get; init; }
    public double IntervalMin { get; init; }
    public double IntervalMax { get; init; }
    public bool Available => Valid != 0 && !Overflow;
    public bool Partial => Valid != Total;
}
public sealed record ProcessGroup
{
    public GroupKey Key { get; init; }
    public string Label { get; init; } = "";
    public int[] Members { get; init; } = Array.Empty<int>();
    public GroupSum[] Metric { get; init; } = Array.Empty<GroupSum>();
    public bool Expanded { get; init; }
    public ulong ObservedMin { get; init; }
    public ulong ObservedMax { get; init; }
    public int Historical { get; init; }
    public int ScopeSplits { get; init; }
}
public sealed record Snapshot
{
    public uint Session { get; init; }
    public ulong Sequence { get; init; }
    public ulong MeasuredUtc { get; init; }
    public ulong ForegroundObservedUtc { get; init; }
    public uint ForegroundPid { get; init; }
    public ProcessRow[] Rows { get; init; } = Array.Empty<ProcessRow>();
    public ProcessGroup[] Groups { get; init; } = Array.Empty<ProcessGroup>();
    public double CollectMs { get; init; }
    public double StoreMs { get; init; }
    public double GroupingMs { get; init; }
    public int CompletedWindows { get; init; }
    public bool Synthetic { get; init; }
    public bool StorageError { get; init; }
    public bool RetentionPending { get; init; }
    public bool CollectorError { get; init; }
    public bool GroupingFailed { get; init; }
    // 편집은 적용됐지만 그 지정을 담은 native 목록이 아직 재구성되지 않았다.
    // 이 값은 native 성공 갱신/새 세션에서만 해제되며 UI 재렌더와는 무관하다.
    public bool GroupRefreshPending { get; init; }
    public bool Paused { get; init; }
    public string Status { get; init; } = "";
    public ulong FeedbackRevision { get; init; }
    public ulong TargetId { get; init; }
    public string TargetLabel { get; init; } = "";
}
public sealed class EngineException : Exception { public EngineException(string message) : base(message) { } }

public sealed record NotificationEvidence(ulong Start, ulong End, double Current, double Median, double Threshold, double Duration, uint Streak, ulong CandidateSince, int Reason, uint RecurrenceCount);
public sealed record NotificationMember(ProcessKey Key, uint Metrics, NotificationEvidence[] Metric, int RowIndex=-1, string ValidationError="");
public sealed record NotificationEvent(ulong Id, uint Session, long ProgramId, string Name, uint Metrics, uint OmittedCount, NotificationMember[] Members, NativeNotification Native)
{
    public int Cause => Members.Length == 0 ? 0 : FirstMetric(Members[0].Metrics);
    public static int FirstMetric(uint mask) { for(int i=0;i<4;i++) if((mask&(1u<<i))!=0) return i; return 0; }
}

// Shell wrapper에 전달하는 고정 배치 값 복사본이다. native DLL은 managed 주소를
// 보관하지 않으며 가변 문자열도 이 값 안의 정해진 공간에 복사한다.
[StructLayout(LayoutKind.Sequential)]
public struct NativeNotificationEvidence
{
    public ulong Start, End;
    public double Current, Median, Threshold, Duration;
    public uint Streak;
}
[StructLayout(LayoutKind.Sequential)]
public struct NativeNotificationMember
{
    public uint Pid, Metrics;
    public ulong Creation;
    [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public NativeNotificationEvidence[] Metric;
    [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public ulong[] CandidateSince;
    [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public int[] Reason;
    [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public uint[] RecurrenceCount;
}
[StructLayout(LayoutKind.Sequential)]
public struct NativeNotification
{
    public ulong Id, QueuedMs;
    public uint Session, Metrics, MemberCount, OmittedCount, OmittedMetrics;
    public long Program;
    [MarshalAs(UnmanagedType.ByValArray,SizeConst=160)] public byte[] Name;
    [MarshalAs(UnmanagedType.ByValArray,SizeConst=8)] public NativeNotificationMember[] Members;
}

// 생성·호출·해제를 같은 worker에서 직렬 수행한다. 반환 DTO는 모든 문자열과 값을
// 소유하므로 native view의 수명과 분리해 WPF Dispatcher에 전달할 수 있다.
public sealed class Engine : IDisposable
{
    private IntPtr handle;
    private readonly int ownerThread = Environment.CurrentManagedThreadId;
    public Snapshot? Current { get; private set; }
    public bool IsNotificationDemo { get; private init; }
    internal bool Failed => Native.bridge_failed(handle)!=0;
    public string LastValidationError { get; private set; } = "";
    private Engine(IntPtr value) { handle=value; }
    public static Engine Open(string dbPath, bool synthetic=false) => Create(dbPath,synthetic?1:0);
    public static Engine OpenNotificationDemo() => Create("",2);
    private static Engine Create(string path,int mode)
    {
        VerifyAbi();
        var error=new byte[512];
        IntPtr p=Native.bridge_open(path,mode,error,(nuint)error.Length);
        if(p==IntPtr.Zero) throw new EngineException(Utf8(error));
        return new Engine(p) { IsNotificationDemo=mode==2 };
    }
    private void Check()
    {
        if(handle==IntPtr.Zero) throw new ObjectDisposedException(nameof(Engine));
        if(Environment.CurrentManagedThreadId!=ownerThread) throw new InvalidOperationException("Engine must be called and disposed on its dedicated worker thread.");
    }
    private EngineException Error() => new(Marshal.PtrToStringUTF8(Native.bridge_error(handle))??"Native engine operation failed");
    public Snapshot Collect() { Check(); return CopyRequired(Native.bridge_collect(handle)); }
    public void Pause() { Check(); if(Native.bridge_pause(handle)==0) throw Error(); if(Current!=null) Current=Current with { Paused=true }; }
    public void Resume() { Check(); if(Native.bridge_resume(handle)==0) throw Error(); Current=null; }
    public Snapshot Edit(GroupAction action, ProcessKey process=default, GroupKey group=default)
    {
        Check(); if(Current==null) throw new EngineException("No current snapshot");
        return CopyRequired(Native.bridge_edit(handle,Current.Session,(int)action,process.Pid,process.Creation,ToNative(group)));
    }
    public Snapshot Edit(uint session, GroupAction action, ProcessKey process=default, GroupKey group=default)
    {
        Check(); return CopyRequired(Native.bridge_edit(handle,session,(int)action,process.Pid,process.Creation,ToNative(group)));
    }
    public Snapshot Feedback(ProcessKey process, FeedbackKind kind) => Feedback(Current?.Session??0,process,Current?.Rows.FirstOrDefault(r=>r.Key==process)?.ProgramId??0,kind);
    public Snapshot Feedback(uint session, ProcessKey process, long programId, FeedbackKind kind)
    {
        Check();
        if(Native.bridge_feedback(handle,session,process.Pid,process.Creation,programId,(int)kind)==0) throw Error();
        return CopyRequired(Native.bridge_current(handle));
    }
    public void SetNotifications(bool enabled) { Check(); Native.bridge_notifications_enable(handle,enabled?1:0); }
    public NotificationEvent? BeginNotification() { Check(); return CopyNotification(Native.bridge_notification_begin(handle)); }
    public NotificationEvent? DispatchNotification(ulong id) { Check(); return CopyNotification(Native.bridge_notification_dispatch(handle,id)); }
    public void ReportNotification(NotificationEvent e,bool accepted) => ReportNotification(e.Id,accepted);
    public void ReportNotification(ulong id,bool accepted) { Check(); Native.bridge_notification_result(handle,id,accepted?1:0); }
    public void RetireNotification(ulong id) { Check(); Native.bridge_notification_retire(handle,id); }
    // 알림 당시 수치를 복사한 뒤 구성원마다 현재 인스턴스를 다시 확인한다. 확인 실패는
    // RowIndex=-1로 남기되 당시 근거는 유지하여 WindowController가 별도로 보여 줄 수 있다.
    public NotificationEvent? ClickNotification(ulong id)
    {
        Check();
        var ev=CopyNotification(Native.bridge_notification_click(handle,id));
        if(ev==null) return null;
        var members=new NotificationMember[ev.Members.Length];
        for(int i=0;i<members.Length;i++)
        {
            long row=Native.bridge_notification_validate(handle,id,ev.Session,(uint)i);
            members[i]=ev.Members[i] with { RowIndex=(int)row, ValidationError=row<0?Error().Message:"" };
        }
        return ev with { Members=members };
    }
    public int ValidateMember(NotificationEvent copy,int index)
    {
        Check();
        if(index<0 || index>=copy.Members.Length) { LastValidationError="알림 구성원을 확인할 수 없습니다."; return -1; }
        long row=Native.bridge_notification_validate(handle,copy.Id,copy.Session,(uint)index);
        LastValidationError=row<0?Error().Message:"";
        return checked((int)row);
    }
    public Snapshot InjectNotificationDemo() { Check(); return CopyRequired(Native.bridge_notification_demo(handle)); }
    public StorageStats GetStorageStats()
    {
        Check(); if(Native.bridge_storage_stats(handle,out StorageStats stats)==0) throw Error(); return stats;
    }
    // 기존 호출자는 대상 변경도 예외로 받는다. worker는 아래 Try 경로로 대상 변경만
    // 조용히 폐기하며, 정상 조회의 준비 중/NO_WINDOW는 여전히 네 지표의 State로 받는다.
    public RecentMetricReport[] GetRecentComparison(uint session, ulong sequence, ProcessKey process, long program, string path)
    {
        if(!TryGetRecentComparison(session,sequence,process,program,path,out var result)) throw Error();
        return result;
    }
    // 세션·회차·PID+생성시각·경로가 바뀌었을 때만 false다. DB 오류와 잘못된 요청은
    // 이번 native 오류로 예외를 던지고, 성공할 때만 native 값 사본을 managed가 소유한다.
    internal bool TryGetRecentComparison(uint session, ulong sequence, ProcessKey process, long program, string path, out RecentMetricReport[] reports)
    {
        Check();
        reports=[];
        var status=Native.bridge_recent_result(handle,session,sequence,process.Pid,process.Creation,program,path,out NativeRecentComparison result);
        if(status==NativeRecentResult.TargetChanged) return false;
        if(status==NativeRecentResult.Error) throw Error();
        if(status!=NativeRecentResult.Success) throw new EngineException("Unknown native recent-comparison result");
        reports=result.Metric.Select(m=>new RecentMetricReport {State=m.State,Count=m.Count,AnchorWindowId=m.AnchorWindowId,
            Current=m.Current,Median=m.Median,Duration=m.Duration,Start=m.Start,End=m.End,
            HistoryStart=m.HistoryStart,HistoryEnd=m.HistoryEnd}).ToArray();
        return true;
    }
    private enum NativeRecentResult { Error=0, Success=1, TargetChanged=2 }
    public void Dispose()
    {
        if(handle==IntPtr.Zero) return;
        Check(); Native.bridge_close(handle); handle=IntPtr.Zero; Current=null;
    }
    private static string Utf8(byte[]? bytes)
    {
        if(bytes==null) return "";
        int n=Array.IndexOf(bytes,(byte)0); return Encoding.UTF8.GetString(bytes,0,n<0?bytes.Length:n);
    }
    private static string Str(IntPtr value) => value==IntPtr.Zero?"":Marshal.PtrToStringUTF8(value)??"";
    private static T Read<T>(IntPtr p,int index=0) where T:struct => Marshal.PtrToStructure<T>(IntPtr.Add(p,checked(index*Marshal.SizeOf<T>())));
    private static NativeGroupKey ToNative(GroupKey k) => new() { Kind=k.Kind,Anchor=new() { Pid=k.Anchor.Pid,Creation=k.Anchor.Creation },Id=k.Id };
    private static GroupKey FromNative(NativeGroupKey k) => new(k.Kind,new(k.Anchor.Pid,k.Anchor.Creation),k.Id);
    private static MetricReport Copy(NativeMetric m, CompletedMetricEvidence? completed) => new()
    {
        State=(MetricState)m.State,Completed=completed,Current=m.Current,Median=m.Median,Mad=m.Mad,Threshold=m.Threshold,CoveredSeconds=m.Covered,
        Count=m.Count,Days=m.Days,Minutes=m.Minutes,Streak=m.Streak,ValidSamples=m.ValidSamples,
        Start=m.Start,End=m.End,Duration=m.Duration,BaselineEligible=m.Eligible!=0,Reason=Utf8(m.Reason)
    };
    // BridgeView와 중첩 배열·문자열은 native가 소유한다. view가 유효한 동안 worker에서
    // 읽기 전용 조회만 사용하며 다음 상태 변경 전에 복사를 마친다. 완성한 managed 배열과
    // 문자열을 Current에 함께 교체하므로 다음 native 수집이 이전 UI frame을 해제하지 않는다.
    // NULL 반환은 native 오류로 던지고, Current를 빈 정상 snapshot으로 바꾸지 않는다.
    private Snapshot CopyRequired(IntPtr pointer)
    {
        if(pointer==IntPtr.Zero) throw Error();
        var v=Read<NativeView>(pointer);
        if(v.Snapshot==IntPtr.Zero) throw new EngineException("No current snapshot");
        var s=Read<NativeSnapshot>(v.Snapshot);
        var rows=new ProcessRow[checked((int)s.Count)];
        // 전체 보고서·문자열 대신 행당 128byte의 완료 근거를 한 번에 복사한다.
        // blittable 배열을 쓰며 세션·회차와 행 순서는 native에서 대조한다.
        var completedWindows=new NativeCompletedWindow[rows.Length];
        if(Native.bridge_completed_windows(handle,v.Session,s.Sequence,completedWindows,(nuint)completedWindows.Length)==0) throw Error();
        var display=v.Display!=IntPtr.Zero?Read<NativeDisplayBatch>(v.Display):default;
        var relations=v.Relations!=IntPtr.Zero?Read<NativeRelations>(v.Relations):default;
        for(int i=0;i<rows.Length;i++)
        {
            var n=Read<NativeRow>(s.Rows,i); var r=n.Raw;
            var d=i<(int)display.Count?Read<NativeDisplay>(display.Items,i):default;
            bool matches=d.Pid==r.Pid && d.Creation==r.Creation && d.Measured==r.Measured;
            var relation=i<(int)relations.Count?Read<NativeRelation>(relations.Items,i):default;
            ulong privateWs=v.PrivateWorkingSet==IntPtr.Zero?ulong.MaxValue:unchecked((ulong)Marshal.ReadInt64(v.PrivateWorkingSet,i*8));
            rows[i]=new ProcessRow
            {
                Key=new(r.Pid,r.Creation),Name=Str(n.Name),Path=Str(n.Path),SnapshotName=matches?Str(d.SnapshotName):"",
                NameFromList=matches && d.NameSource==2,Valid=(ResourceValid)r.Valid,MeasuredUtc=r.Measured,ProgramId=n.Program,
                Cpu=r.Cpu,Memory=r.Memory,PrivateWorkingSet=privateWs==ulong.MaxValue?null:privateWs,Read=r.Read,Write=r.Write,
                Elapsed=r.Elapsed,ObservationInterval=r.ObservationInterval,CpuInterval=r.CpuInterval,IoInterval=r.IoInterval,
                Foreground=r.Foreground,CpuStatus=r.CpuStatus,ReadStatus=r.ReadStatus,WriteStatus=r.WriteStatus,
                OpenError=r.OpenError,TimesError=r.TimesError,MemoryError=r.MemoryError,IoError=r.IoError,NameError=r.NameError,
                PathError=n.PathError,Metric=n.Metric.Select((m,index)=>Copy(m,CopyCompleted(completedWindows[i],index))).ToArray(),Feedback=(FeedbackKind)n.Feedback,FeedbackExpires=n.FeedbackExpires,
                QueryAttempts=matches?d.Attempted:0,ExitError=matches?d.ExitError:0,ExitCode=matches?d.ExitCode:0,
                ExitObserved=matches && d.ExitObserved!=0,SpecialProcess=matches?d.Special:0,RelationGrade=relation.Grade,RelationCut=relation.Cut
            };
        }
        var ng=v.Groups!=IntPtr.Zero?Read<NativeGroups>(v.Groups):default;
        var groups=new ProcessGroup[checked((int)ng.Count)];
        for(int i=0;i<groups.Length;i++)
        {
            var g=Read<NativeGroup>(ng.Items,i); var members=new int[checked((int)g.Count)];
            for(int j=0;j<members.Length;j++) members[j]=checked((int)Marshal.ReadInt64(ng.Members,checked(((int)g.First+j)*8)));
            groups[i]=new ProcessGroup { Key=FromNative(g.Key),Label=Utf8(g.Label),Members=members,
                Metric=g.Metric.Select(m=>new GroupSum { Value=m.Value,Bytes=m.Bytes,Valid=checked((int)m.Valid),Total=checked((int)m.Total),Overflow=m.Overflow!=0,IntervalMin=m.IntervalMin,IntervalMax=m.IntervalMax }).ToArray(),
                Expanded=Native.bridge_group_expanded(handle,g.Key)!=0,ObservedMin=g.ObservedMin,ObservedMax=g.ObservedMax,Historical=(int)g.Historical,ScopeSplits=(int)g.ScopeSplits };
        }
        Current=new Snapshot { Session=v.Session,Sequence=s.Sequence,MeasuredUtc=s.Measured,ForegroundObservedUtc=s.ForegroundObserved,ForegroundPid=s.ForegroundPid,
            Rows=rows,Groups=groups,CollectMs=s.CollectMs,StoreMs=s.StoreMs,GroupingMs=v.GroupingMs,CompletedWindows=(int)s.Windows,
            Synthetic=s.Synthetic!=0,StorageError=s.StorageError!=0,CollectorError=s.CollectorError!=0,GroupingFailed=v.GroupingFailed!=0,Paused=v.Paused!=0,
            RetentionPending=v.RetentionPending!=0,GroupRefreshPending=Native.bridge_group_refresh_pending(handle)!=0,
            Status=Utf8(s.Status),FeedbackRevision=v.FeedbackRevision,TargetId=v.TargetId,TargetLabel=Str(v.TargetLabel) };
        return Current;
    }
    private static CompletedMetricEvidence? CopyCompleted(NativeCompletedWindow window,int metric)
    {
        uint state=(window.States>>(metric*4))&15;
        if(state>5) throw new EngineException("Invalid completed-window state");
        if(state==0) return null;
        if(window.End<=window.Start) throw new EngineException("Invalid completed-window interval");
        NativeCompletedMetric values=metric switch {0=>window.Cpu,1=>window.Ram,2=>window.Read,_=>window.Write};
        return new((MetricState)(state-1),values.Average,values.Median,values.Upper,window.Start,window.End,window.Duration);
    }
    private static NotificationEvent? CopyNotification(IntPtr pointer)
    {
        if(pointer==IntPtr.Zero) return null;
        var n=Read<NativeNotification>(pointer);
        if(n.MemberCount>8) throw new EngineException("Invalid native notification member count");
        var members=new NotificationMember[n.MemberCount];
        for(int i=0;i<members.Length;i++)
        {
            var m=n.Members[i]; var metric=new NotificationEvidence[4];
            for(int j=0;j<4;j++) { var e=m.Metric[j]; metric[j]=new(e.Start,e.End,e.Current,e.Median,e.Threshold,e.Duration,e.Streak,m.CandidateSince[j],m.Reason[j],m.RecurrenceCount[j]); }
            members[i]=new(new(m.Pid,m.Creation),m.Metrics,metric);
        }
        return new(n.Id,n.Session,n.Program,Utf8(n.Name),n.Metrics,n.OmittedCount,members,n);
    }
    // 포인터를 자료형으로 읽기 전에 x64와 C/관리 구조체 크기를 대조한다. 필드 계약을
    // 바꿀 때는 아래 Native 자료형과 native/bridge.c::bridge_sizeof를 함께 확인한다.
    private static void VerifyAbi()
    {
        if(IntPtr.Size!=8) throw new PlatformNotSupportedException("MonitorBridge requires x64");
        Type[] types={typeof(NativeView),typeof(NativeRaw),typeof(NativeMetric),typeof(NativeRow),typeof(NativeSnapshot),typeof(NativeDisplay),typeof(NativeGroup),typeof(NativeGroups),typeof(NativeGroupKey),typeof(NativeNotification),typeof(NativeNotificationMember),typeof(NativeRecentMetric),typeof(NativeRecentComparison),typeof(NativeCompletedWindow)};
        for(int i=0;i<types.Length;i++) if(Native.bridge_sizeof(i)!=(nuint)Marshal.SizeOf(types[i]))
            throw new EngineException($"Native ABI mismatch: {types[i].Name} native={Native.bridge_sizeof(i)}, managed={Marshal.SizeOf(types[i])}");
    }
    [StructLayout(LayoutKind.Sequential)] public struct StorageStats { public ulong RawRows,Windows,MetricRows,PageBytes,RawPageBytes,AggregatePageBytes; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeCompletedMetric
    {
        public double Average,Median,Upper;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeCompletedWindow
    {
        public ulong Start,End; public double Duration; public uint States;
        public NativeCompletedMetric Cpu,Ram,Read,Write;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRecentMetric
    {
        public uint State,Count; public long AnchorWindowId;
        public double Current,Median,Duration; public ulong Start,End,HistoryStart,HistoryEnd;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRecentComparison
    {
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public NativeRecentMetric[] Metric;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeView
    {
        public IntPtr Snapshot,Display,Relations,Groups; public double GroupingMs; public uint Session; public int GroupingFailed,Paused;
        public ulong FeedbackRevision,TargetId; public IntPtr TargetLabel,PrivateWorkingSet; public int RetentionPending;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRaw
    {
        public ulong Measured; public uint Pid; public IntPtr ProcessName; public ulong Creation; public uint Valid;
        public double Cpu; public ulong Memory; public double Read,Write,Elapsed,ObservationInterval,CpuInterval,IoInterval;
        public int Foreground,CpuStatus,ReadStatus,WriteStatus;
        public uint OpenError,TimesError,MemoryError,IoError,NameError;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeMetric
    {
        public int State; public double Current,Median,Mad,Threshold,Covered;
        public uint Count,Days,Minutes,Streak,ValidSamples; public ulong Start,End; public double Duration; public int Eligible;
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=96)] public byte[] Reason;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRow
    {
        public NativeRaw Raw; public IntPtr Name,Path; public long Program; public uint PathError;
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public NativeMetric[] Metric;
        public int Feedback; public ulong FeedbackExpires;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeSnapshot
    {
        public IntPtr Rows; public nuint Count; public ulong Measured,Sequence,ForegroundObserved; public uint ForegroundPid;
        public double CollectMs,StoreMs; public nuint Windows; public int Synthetic,StorageError,CollectorError;
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=256)] public byte[] Status;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeDisplayBatch { public IntPtr Items; public nuint Count; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeDisplay
    {
        public uint Pid; public ulong Creation,Measured; public int NameSource; public IntPtr SnapshotName;
        public uint SnapshotNameError,Attempted,ExitError,ExitCode; public int ExitObserved,Special;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeIdentity { public uint Pid; public ulong Creation; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeGroupKey { public int Kind; public NativeIdentity Anchor; public ulong Id; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeSum
    {
        public double Value,IntervalMin,IntervalMax; public ulong Bytes; public nuint Valid,Total; public int Overflow;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeGroup
    {
        public NativeGroupKey Key; [MarshalAs(UnmanagedType.ByValArray,SizeConst=256)] public byte[] Label; public nuint First,Count;
        [MarshalAs(UnmanagedType.ByValArray,SizeConst=4)] public NativeSum[] Metric;
        public ulong ObservedMin,ObservedMax; public nuint ScopeSplits,Historical;
    }
    [StructLayout(LayoutKind.Sequential)] private struct NativeGroups { public IntPtr Items; public nuint Count,RowCount; public IntPtr Members,Owner,Order; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRelations { public IntPtr Items,Order,Index; public nuint Count,Inferred,Historical,Roots; public uint ProbeError; }
    [StructLayout(LayoutKind.Sequential)] private struct NativeRelation
    {
        public uint ParentPid,Error; public int ParentValid; public NativeIdentity Parent; public int Grade,Cut;
        public ulong LastMatched; public nuint DisplayParent,Children,Descendants; public uint Depth;
    }
    private static class Native
    {
        private const string Dll="MonitorBridge.dll";
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_open([MarshalAs(UnmanagedType.LPUTF8Str)]string path,int mode,byte[] error,nuint capacity);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern void bridge_close(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_error(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_failed(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_group_refresh_pending(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_current(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_collect(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_pause(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_resume(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_edit(IntPtr b,uint session,int action,uint pid,ulong creation,NativeGroupKey group);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_feedback(IntPtr b,uint session,uint pid,ulong creation,long program,int kind);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_group_expanded(IntPtr b,NativeGroupKey group);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern void bridge_notifications_enable(IntPtr b,int enabled);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_notification_begin(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_notification_dispatch(IntPtr b,ulong id);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern void bridge_notification_result(IntPtr b,ulong id,int accepted);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern void bridge_notification_retire(IntPtr b,ulong id);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_notification_click(IntPtr b,ulong id);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern long bridge_notification_validate(IntPtr b,ulong id,uint session,uint member);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern IntPtr bridge_notification_demo(IntPtr b);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_storage_stats(IntPtr b,out StorageStats stats);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern NativeRecentResult bridge_recent_result(IntPtr b,uint session,ulong sequence,uint pid,ulong creation,long program,[MarshalAs(UnmanagedType.LPUTF8Str)]string path,out NativeRecentComparison result);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern int bridge_completed_windows(IntPtr b,uint session,ulong sequence,[Out]NativeCompletedWindow[] windows,nuint count);
        [DllImport(Dll,CallingConvention=CallingConvention.Cdecl)] internal static extern nuint bridge_sizeof(int type);
    }
}
