using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace PcMonitor;

// Build에서 native 복사본을 전체·그룹·개별 표시값으로 바꾼다. Compare/Recent/Alert는
// 서로 다른 관측 근거를 보존해 MainWindow에 전달하며 DB 조회나 후보 판정은 하지 않는다.
internal sealed class Projection
{
    public const string Other = "__other";
    private static readonly string[] Palette = ["#218979","#7666B2","#CE8A45","#497FAD","#A86683","#78934E","#41999C","#9B745C"];
    private readonly Dictionary<string,string> colors = new();
    private int nextColor;
    public static string Id(GroupKey key) => $"{key.Kind}/{key.Id}/{key.Anchor.Pid}/{key.Anchor.Creation}";
    public static DateTimeOffset? Time(ulong ft) =>
        ft > 0 && ft <= 2650467743999999999UL ? DateTimeOffset.FromFileTime((long)ft).ToUniversalTime() : null;
    private static double? Number(double value) => double.IsFinite(value) && value >= 0 ? value : null;
    private string Color(string key)
    {
        if (key == Other) return "#788590";
        if (colors.TryGetValue(key, out var value)) return value;
        int n = nextColor++;
        // 기본 팔레트를 넘는 그룹도 행 정렬과 무관하게 고유 색상을 유지한다.
        if (n < Palette.Length) value = Palette[n];
        else {
            double hue = n * 137.50776405 % 360 / 60;
            double x = 0.40 * (1 - Math.Abs(hue % 2 - 1));
            var (r,g,b) = (int)hue switch {
                0 => (.40,x,0d), 1 => (x,.40,0d), 2 => (0d,.40,x),
                3 => (0d,x,.40), 4 => (x,0d,.40), _ => (.40,0d,x)
            };
            value = $"#{(int)((r+.25)*255):X2}{(int)((g+.25)*255):X2}{(int)((b+.25)*255):X2}";
        }
        return colors[key] = value;
    }
    private static UiComparison Compare(MetricReport r,int metric)
    {
        var completed=r.Completed;
        var state=completed?.State ?? r.State;
        ulong start=completed?.Start ?? r.Start,end=completed?.End ?? r.End;
        bool sameWindow=completed==null || (r.Start==start && r.End==end);
        // 부분 평균도 계산된 값이면 보존한다. 다만 학습 중의 예비 통계나 불충분한
        // 새 완료창을 준비된 개인 상한으로 올리지 않으며 옛 창으로 메우지도 않는다.
        return new() {
            Metric=(UiMetric)metric,StateCode=r.State,CompletedStateCode=completed?.State,
            Reason=completed!=null && (!sameWindow || state!=r.State)
                ? "마지막 완료 구간의 근거 · 현재 표본은 별도 표시" : r.Reason,
            Start=Time(start),End=Time(end),
            Average=end>start ? Number(completed?.Average ?? r.Current) : null,
            Median=state>=MetricState.Normal ? Number(completed?.Median ?? r.Median) : null,
            Upper=state>=MetricState.Normal ? Number(completed?.Upper ?? r.Threshold) : null,
            Evidence=(sameWindow
                ? $"완료 구간 이력 {r.Count}개 · {r.Days}일 · {r.Minutes}분 / 완료 구간 관측 {r.CoveredSeconds:0.0}초, 구간 {r.Duration:0.0}초"
                : $"현재 관측과 분리하여 보존한 완료 구간 {completed!.Value.Duration:0.0}초") +
                $" / 현재 연속 초과 {r.Streak}회"
        };
    }
    internal static UiRecentComparison Recent(RecentMetricReport r, int metric) => new() {
        Metric=(UiMetric)metric,State=(RecentComparisonState)r.State,Count=r.Count,
        Average=r.State is 1 or 2 ? Number(r.Current) : null,
        Median=r.State==2 ? Number(r.Median) : null,
        Start=Time(r.Start),End=Time(r.End),HistoryStart=Time(r.HistoryStart),HistoryEnd=Time(r.HistoryEnd),Duration=r.Duration,
        Evidence=$"저장된 유효 완료 구간 · 종료 분당 대표 1개, 최대 30개 · 현재 구간 제외 · 기준 구간 ID {r.AnchorWindowId}"
    };
    public static UiAlert Alert(NotificationMember member,int metric,DateTimeOffset? accepted)
    {
        var e = member.Metric[metric];
        var why = e.Reason switch { 2 => "같은 지표가 다시 상한을 넘었습니다.", 3 => "보류한 변화가 이어졌습니다.",
            4 => "이전 알림 이후 사용량이 더 늘었습니다.", _ => "완료 구간 평균이 개인 기준 상한을 연속으로 넘었습니다." };
        return new() { Metric = (UiMetric)metric, Average = Number(e.Current), Upper = Number(e.Threshold),
            Start = Time(e.Start), End = Time(e.End), AcceptedAt = accepted, Reason = why,
            Evidence = $"{why} 당시 연속 {e.Streak}회 · 실제 구간 {e.Duration:0.0}초 · 재발 {e.RecurrenceCount}회" };
    }

    private static string Feedback(ProcessRow row)
    {
        if (row.Feedback == FeedbackKind.Cancel || Time(row.FeedbackExpires) is not { } expires || expires <= DateTimeOffset.UtcNow) return "";
        return $"{(row.Feedback == FeedbackKind.Ignore ? "1시간 알림 억제" : "예상한 활동")} · {expires.ToLocalTime():MM-dd HH:mm}까지";
    }
    private static string Evidence(ProcessRow r)
    {
        string identity = r.Valid.HasFlag(ResourceValid.Identity) ? $"생성 {Time(r.Key.Creation):yyyy-MM-dd HH:mm:ss.fffffff} UTC" : "프로세스 신원 확인 불가";
        string source = r.NameFromList ? " · 목록에서 읽은 이름" : "";
        return $"PID {r.Key.Pid} · {identity}{source}\n{r.Path}\nCPU 관측 {r.CpuInterval:0.000}초 · I/O 관측 {r.IoInterval:0.000}초" +
            (r.OpenError != 0 ? $" · 프로세스 열기 오류 {r.OpenError}" : "") +
            (r.MemoryError != 0 ? $" · RAM 오류 {r.MemoryError}" : "") +
            (r.PathError != 0 ? $" · 경로 오류 {r.PathError}" : "");
    }

    private sealed record Bucket(string Key,string Name,string Description)
    {
        public readonly List<int> Members = new();
        public readonly List<GroupSum[]> Sums = new();
    }
    private static (double? Value,int Valid) Sum(Bucket group, int metric, Snapshot data)
    {
        if (group.Sums.Count > 0) {
            var sums = group.Sums.Select(s => s[metric]);
            if (sums.Any(s => s.Overflow)) return (null,0);
            int count = sums.Sum(s => s.Valid);
            return (count == 0 ? null : sums.Sum(s => metric == 1 ? (double)s.Bytes : s.Value),count);
        }
        return SumValues(group.Members.Select(i => data.Rows[i].Value(metric)));
    }
    private static (double? Value,int Valid) SumValues(IEnumerable<double?> values)
    {
        int count = 0;
        double sum = 0;
        foreach (double? value in values) if (value.HasValue && double.IsFinite(value.Value)) { ++count; sum += value.Value; }
        return (count != 0 ? sum : null,count);
    }
    // 검색 전 그룹 비율로 시스템 전체를 분할할 수 있을 때만 미귀속·여유 조각을 만든다.
    // 귀속 합이 전체보다 크면 원은 전체/여유만 그리고 관측된 그룹 수치는 그대로 보존한다.
    internal static UiChart Chart(string metric,double? used,List<UiSlice> slices,string detail)
    {
        slices.Sort((a,b) => Nullable.Compare(b.Percent,a.Percent));
        double known = slices.Sum(s => s.Percent ?? 0);
        bool partition = used.HasValue && known <= used.Value + 1e-10;
        if (partition) {
            double remainder = Math.Max(0,used!.Value-known);
            if (remainder > 0) slices.Add(new UiSlice {Key="__unattributed",Name="미귀속",Color="#ABB9BD",Percent=remainder,DrawPercent=remainder,Unattributed=true});
            slices.Add(new UiSlice {Key="__free",Name="여유",Color="#DFE6E1",Percent=100-used,DrawPercent=100-used.Value,Free=true});
        }
        string note = !used.HasValue ? "전체 측정 대기 또는 사용 불가" : !partition ? "측정 시차로 귀속 합이 전체를 초과함 · 원은 전체/여유" : "";
        return new UiChart { MetricName=metric, Total=used, Slices=slices, CanPartition=partition, Note=note, Explanation=detail + (note.Length==0?"":" · "+note) };
    }
    // native 그룹을 표시 묶음으로 옮긴 뒤 행, 그룹 합계, 도넛 순서로 만든다. 목록 RAM은
    // Working Set, 도넛 RAM은 전용 Working Set이며 유효 구성원 수를 각 지표에 함께 전달한다.
    public UiSnapshot Build(Snapshot data,SystemReading sys,bool enable,
        IReadOnlyDictionary<(ProcessKey,long),UiAlert[]> alerts)
    {
        var buckets = new Dictionary<string,Bucket>();
        string[] memberships = new string[data.Rows.Length];
        foreach (var g in data.Groups) {
            bool other = g.Key.Kind == 2 || (g.Key.Kind == 0 && g.Members.Length <= 1);
            string key = other ? Other : Id(g.Key);
            if (!buckets.TryGetValue(key, out var b)) {
                string description = other ? "미지정 및 분리된 구성원" : g.Key.Kind == 1 ? "사용자 지정 그룹" : "현재 실행 관계로 추정한 그룹";
                string label=other ? "기타" : g.Label;
                if(!other && g.Key.Kind==0) {
                    var anchor=g.Members.Select(i=>data.Rows[i]).FirstOrDefault(r=>r.Key==g.Key.Anchor);
                    if(anchor!=null && !anchor.Valid.HasFlag(ResourceValid.Name)) label=anchor.DisplayName;
                }
                buckets.Add(key,b = new(key,label,description));
            }
            b.Members.AddRange(g.Members);
            if (g.Metric.Length == 4) b.Sums.Add(g.Metric);
            foreach (int i in g.Members) memberships[i] = key;
        }
        for (int i=0;i<memberships.Length;i++) if (memberships[i] == null) {
            if (!buckets.TryGetValue(Other,out var b)) buckets.Add(Other,b = new(Other,"기타","소속 확인 불가"));
            b.Members.Add(i); memberships[i]=Other;
        }
        foreach (var missing in colors.Keys.Where(k=>!buckets.ContainsKey(k)).ToArray()) colors.Remove(missing);
        var rows = new UiRow[data.Rows.Length];
        var observedPids=new HashSet<uint>(); var ambiguousPids=new HashSet<uint>();
        foreach(var row in data.Rows) if(!observedPids.Add(row.Key.Pid)) ambiguousPids.Add(row.Key.Pid);
        for (int i=0;i<rows.Length;i++) {
            var r=data.Rows[i];
            bool identified=r.Key.IsKnown && r.Valid.HasFlag(ResourceValid.Identity) && !ambiguousPids.Contains(r.Key.Pid);
            var copies=identified ? alerts.GetValueOrDefault((r.Key,r.ProgramId)) ?? [] : [];
            rows[i]=new UiRow { Pid=r.Key.Pid, Creation=r.Key.Creation, IdentityValid=identified,
                GroupKey=memberships[i], Name=r.DisplayName, Path=r.Path, ProgramId=r.ProgramId.ToString(CultureInfo.InvariantCulture),
                CpuPercent=r.Value(0), WorkingSetBytes=r.Value(1), RamPercent=sys.MemoryPercent(r.Value(1)), GpuPercent=identified ? sys.GpuShare(r) : null,
                ReadBytesPerSecond=r.Value(2), WriteBytesPerSecond=r.Value(3), ObservedAt=Time(r.MeasuredUtc),
                Comparisons=r.Metric.Select(Compare).ToArray(), Alert=copies.FirstOrDefault(), Alerts=copies,
                Feedback=Feedback(r), Evidence=Evidence(r) };
        }
        var groups=new List<UiGroup>();
        var cpuSlices=new List<UiSlice>(); var gpuSlices=new List<UiSlice>(); var ramSlices=new List<UiSlice>();
        foreach (var b in buckets.Values) {
            (double? cpu,int cv)=Sum(b,0,data); (double? ws,int wv)=Sum(b,1,data);
            (double? read,int rv)=Sum(b,2,data); (double? write,int wtv)=Sum(b,3,data);
            (double? gpu,int gv)=SumValues(b.Members.Select(i=>rows[i].GpuPercent));
            (double? exclusive,int pv)=SumValues(b.Members.Select(i=>(double?)data.Rows[i].PrivateWorkingSet));
            int n=b.Members.Count;
            string color=Color(b.Key);
            UiSlice Slice(double? percent,int valid) => new() {Key=b.Key,Name=b.Name,Color=color,Percent=percent,DrawPercent=percent??0,Valid=valid,Total=n};
            cpuSlices.Add(Slice(cpu,cv)); gpuSlices.Add(Slice(gpu,gv)); ramSlices.Add(Slice(sys.MemoryPercent(exclusive),pv));
            groups.Add(new UiGroup {Key=b.Key,Name=b.Name,Color=color,Description=b.Description,MemberCount=n,
                CpuPercent=cpu,CpuValid=cv,GpuPercent=gpu,GpuValid=gv,RamPercent=sys.MemoryPercent(ws),RamValid=wv,WorkingSetBytes=ws,
                ReadBytesPerSecond=read,WriteBytesPerSecond=write,ReadValid=rv,WriteValid=wtv,AlertCount=b.Members.Count(i=>rows[i].Alert!=null),
                Evidence=$"유효/전체: CPU {cv}/{n}, GPU {gv}/{n}, Working Set {wv}/{n}, 전용 RAM {pv}/{n}, 읽기 {rv}/{n}, 쓰기 {wtv}/{n}" });
        }
        string at=Time(sys.Utc)?.ToLocalTime().ToString("HH:mm:ss.fff") ?? "—";
        string gpuInfo=sys.Gpu.HasValue ? $"{sys.Engine} · {Time(sys.GpuStart)?.ToLocalTime():HH:mm:ss.fff}–{Time(sys.GpuEnd)?.ToLocalTime():HH:mm:ss.fff}" : $"카운터 상태 0x{sys.GpuStatus:X8}";
        int missingRam=rows.Count(r=>!r.WorkingSetBytes.HasValue);
        return new UiSnapshot {SessionEpoch=data.Session,Sequence=(long)data.Sequence,ObservedAt=Time(data.MeasuredUtc),
            Paused=data.Paused,IsSynthetic=data.Synthetic,NotificationsEnabled=enable,RetentionPending=data.RetentionPending,
            GroupRefreshPending=data.GroupRefreshPending,
            Status=data.Paused?"일시정지":data.Synthetic?"합성 데이터":"실시간 수집 중",
            Footer=$"개별 RAM {missingRam}개 측정 불가"+(data.GroupingFailed?" · 그룹 관계 확인 불가":""),
            Rows=rows,Groups=groups, Cpu=Chart("CPU",sys.Cpu,cpuSlices,$"전체 논리 CPU의 비유휴 시간 · 시스템 관측 {at}. 그룹은 구성원별 QPC 구간의 현재 비율 합."),
            Gpu=Chart("GPU",sys.Gpu,gpuSlices,$"가장 바쁜 단일 GPU 엔진의 관측 점유율 · {gpuInfo}. 해당 엔진에서 같은 실행 인스턴스로 확인한 PID만 귀속."),
            Ram=Chart("RAM",sys.Ram,ramSlices,$"물리 메모리 {(sys.Capacity/1e9):0.0} GB · 시스템 관측 {at}. 원은 전용 Working Set, 목록과 개인 기준은 공유 페이지를 포함한 Working Set. 나머지 사용량은 미귀속.") };
    }
}
