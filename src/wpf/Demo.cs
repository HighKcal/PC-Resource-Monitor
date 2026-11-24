using System;
using System.Collections.Generic;
using System.Linq;

namespace PcMonitor;

// MonitorLoop가 화면 검수 모드에서 발행하는 고정 합성 자료다. 실제 수집 Engine·SQLite·Shell에 전달하지 않는다.
internal static class Demo
{
    public const string Renderer = "2140:134200000000000002";
    // 작은 그룹 탐색을 반복 확인하는 화면 예시다. 첫 그룹의 알림 구간은 고정하고,
    // 한 그룹의 현재 점유율만 두 표본씩 4% 경계의 아래와 위를 오가게 한다.
    public static UiSnapshot SmallGroupsFrame(uint session,long sequence,bool paused,bool enabled)
    {
        var frame = Frame(session, sequence, paused, enabled);
        var at = frame.ObservedAt!.Value.AddSeconds(3 * Math.Max(0, sequence - 1));
        var groups = new List<UiGroup>();
        var rows = new List<UiRow>();
        void Add(string key, string name, string color, double cpu, double? gpu, double? ram)
        {
            var members = new List<UiRow>();
            if (key == "canvas")
            {
                members.AddRange(frame.Rows.Where(r => r.GroupKey == key).Select(r => r with { ObservedAt = at }));
            }
            else
            {
                int i = rows.Count;
                members.Add(new UiRow { Pid = (uint)(3200+i), Creation = 134200000000001000UL+(ulong)i,
                    Name = key == Projection.Other ? "분리한 구성원" : name + " 실행기", GroupKey = key,
                    CpuPercent = cpu, GpuPercent = gpu, RamPercent = ram, WorkingSetBytes = ram*12e9/100,
                    PrivateRamPercent = ram, PrivateWorkingSetBytes = ram*12e9/100,
                    ReadBytesPerSecond = 0, WriteBytesPerSecond = 0, ObservedAt = at,
                    ProgramId = (i+10).ToString(), Evidence = "합성 화면 검수 자료" });
            }
            rows.AddRange(members);
            groups.Add(new UiGroup { Key = key, Name = name, Color = color, Description = "합성 화면 검수 자료",
                MemberCount = members.Count, AlertCount = members.Count(r => r.HasAlert),
                CpuPercent = cpu, GpuPercent = gpu, RamPercent = ram, WorkingSetBytes = ram*12e9/100,
                PrivateRamPercent = ram,
                CpuValid = members.Count, GpuValid = members.Count(r => r.GpuPercent.HasValue),
                RamValid = members.Count(r => r.RamPercent.HasValue), PrivateRamValid = members.Count(r => r.PrivateWorkingSetBytes.HasValue),
                ReadValid = members.Count, WriteValid = members.Count,
                ReadBytesPerSecond = members.Sum(r => r.ReadBytesPerSecond), WriteBytesPerSecond = members.Sum(r => r.WriteBytesPerSecond) });
        }
        Add("canvas", "라온 캔버스", "#218979", 32, 29, 28);
        Add("media", "누리 미디어", "#7666B2", 12, 48, 35);
        Add("studio", "모아 스튜디오", "#5D82AA", 4, 4, 4);
        Add("photo", "다온 사진", "#B57D3F", Math.Max(0,sequence-1)%4 < 2 ? 3.9 : 4.1, .2, 0);
        Add("audio", "소리 작업실", "#508CA1", 3.1, 0, 3.1);
        Add("archive", "보리 보관함", "#98805E", 2.4, 0, 2.4);
        Add("mail", "이음 메일", "#4A8575", .7, 0, .7);
        Add("calendar", "하루 달력", "#B6738C", .5, 0, null);
        Add("note", "나래 노트", "#788C50", .3, null, null);
        Add(Projection.Other, "기타", "#788590", 2, 1.4, 1);
        UiChart Chart(string metric, double total, Func<UiGroup,double?> share, Func<UiGroup,int> valid) =>
            Projection.Chart(metric, total, groups.Select(g => new UiSlice { Key = g.Key, Name = g.Name, Color = g.Color,
                Percent = share(g), DrawPercent = share(g) ?? 0, Valid = valid(g), Total = g.MemberCount }).ToList(),
                "합성 측정 예시 · RAM 용량 12 GB, 측정된 Working Set은 전용 페이지");
        return frame with { Rows = rows, Groups = groups, ObservedAt = at,
            Cpu = Chart("CPU", 64, g => g.CpuPercent, g => g.CpuValid),
            Gpu = Chart("GPU", 84, g => g.GpuPercent, g => g.GpuValid),
            Ram = Chart("RAM", 75, g => g.PrivateRamPercent, g => g.PrivateRamValid) };
    }

    public static UiSnapshot Frame(uint session,long sequence,bool paused,bool enabled)
    {
        var when=new DateTimeOffset(2026,11,22,9,43,0,TimeSpan.FromHours(9));
        var alert=new UiAlert {Metric=UiMetric.Ram,Average=1.8e9,Upper=1.0e9,Start=when.AddMinutes(-3),End=when.AddMinutes(-2),
            AcceptedAt=when.AddMinutes(-2),Reason="RAM 구간 평균이 개인 기준 상한을 연속으로 넘었습니다.",Evidence="합성 완료 구간 · RAM · 연속 3회 초과"};
        UiRow Row(int i,string key,string name,double cpu,double gpu,double? ram,double read,double write,bool hasAlert=false) => new() {
            Pid=(uint)(2140+i),Creation=134200000000000002UL+(ulong)i,Name=name,GroupKey=key,
            CpuPercent=cpu,GpuPercent=gpu,RamPercent=ram,WorkingSetBytes=ram*12e9/100,ReadBytesPerSecond=read,WriteBytesPerSecond=write,
            PrivateRamPercent=ram,PrivateWorkingSetBytes=ram*12e9/100,
            ObservedAt=when,Alert=hasAlert?alert:null,Alerts=hasAlert?[alert]:[],ProgramId=(i+1).ToString(),Evidence="합성 화면 검수 자료"
        };
        UiRow[] rows=[Row(0,"canvas","라온 렌더러",8,24,5,1.24e6,0.08e6,true),Row(1,"canvas","라온 편집기",24,5,23,0,12000),
            Row(2,"media","누리 플레이어",18,36,24,3.2e6,0),Row(3,"media","누리 코덱",4,10,10,0.1e6,0),
            Row(4,Projection.Other,"모아 노트",5,0,null,0,0),Row(5,Projection.Other,"이음 메일",5,9,null,0,0)];
        string[] keys=["canvas","media",Projection.Other], names=["라온 캔버스","누리 미디어","기타"], colors=["#218979","#7666B2","#788590"];
        var groups=keys.Select((key,i)=> {
            var members=rows.Where(r=>r.GroupKey==key).ToArray();
            return new UiGroup {Key=key,Name=names[i],Color=colors[i],MemberCount=members.Length,
                CpuPercent=members.Sum(r=>r.CpuPercent),GpuPercent=members.Sum(r=>r.GpuPercent),RamPercent=i==2?null:members.Sum(r=>r.RamPercent),
                WorkingSetBytes=i==2?null:members.Sum(r=>r.WorkingSetBytes),ReadBytesPerSecond=members.Sum(r=>r.ReadBytesPerSecond),
                PrivateRamPercent=i==2?null:members.Sum(r=>r.PrivateRamPercent),
                WriteBytesPerSecond=members.Sum(r=>r.WriteBytesPerSecond),ReadValid=2,WriteValid=2,CpuValid=2,GpuValid=2,RamValid=i==2?0:2,
                PrivateRamValid=i==2?0:2,AlertCount=i==0?1:0,Description=i==2?"미지정 2개":"합성 그룹"};
        }).ToArray();
        UiChart Chart(string metric,double total,Func<UiGroup,double?> select,string explanation) =>
            Projection.Chart(metric,total,groups.Select(g=>new UiSlice {Key=g.Key,Name=g.Name,Color=g.Color,
                Percent=select(g),DrawPercent=select(g)??0,Valid=select(g).HasValue?2:0,Total=2}).ToList(),explanation);
        return new UiSnapshot {SessionEpoch=session,Sequence=sequence,ObservedAt=when,Paused=paused,NotificationsEnabled=enabled,IsSynthetic=true,
            Status=paused?"합성 · 일시정지":"합성 데이터",Footer="개별 전용 RAM 2개 측정 불가 · * 일부 구성원의 부분합",Groups=groups,Rows=rows,
            Cpu=Chart("CPU",64,g=>g.CpuPercent,"전체 논리 CPU 용량 기준 · 합성"),
            Gpu=Chart("GPU",84,g=>g.GpuPercent,"단일 GPU 엔진의 측정 구간 기준 · 합성"),
            Ram=Chart("RAM",75,g=>g.PrivateRamPercent,"물리 RAM 12 GB · 목록·도넛은 전용 Working Set. PC 전체는 시스템 물리 RAM 사용률. 이 예시의 측정 가능 행에는 공유 페이지가 없습니다. 미귀속 사용량 13%.") };
    }
}
