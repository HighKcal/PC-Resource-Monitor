using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace PcMonitor;

internal sealed record SystemReading
{
    public ulong Utc, GpuStart, GpuEnd, Capacity, Available;
    public double? Cpu, Gpu;
    public uint GpuStatus;
    public string Engine = "";
    public Dictionary<uint, double> GpuProcesses = new();
    public double? Ram => Capacity > 0 ? 100d * (Capacity - Available) / Capacity : null;
    public double? MemoryPercent(double? bytes) => Capacity > 0 ? bytes * 100d / Capacity : null;

    // PDH 구간은 이번 핸들 기반 프로세스 표본보다 앞선다. 구간 도중이나 이후에
    // 생성된 프로세스에 같은 PID의 이전 사용률을 이어 붙이지 않는다.
    public double? GpuShare(ProcessRow row) =>
        Gpu.HasValue && row.Valid.HasFlag(ResourceValid.Identity) &&
        row.Key.Creation > 0 && row.Key.Creation < GpuStart &&
        row.MeasuredUtc >= GpuEnd && row.MeasuredUtc - GpuEnd <= 150_000_000 &&
        GpuProcesses.TryGetValue(row.Key.Pid, out var rate) ? rate : null;
}

// worker가 native Usage를 소유하고 Read에서 GPU PID 배열까지 복사한다.
// sys_read가 다음 호출에서 배열을 해제하므로 UI에는 이 복사본만 넘긴다.
internal sealed class SystemUsage : IDisposable
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NativeView
    {
        public ulong Utc, Start, End, Capacity, Available;
        public double Cpu, Gpu;
        public IntPtr Pids;
        public uint Count, Status, High, Low, Physical, Engine;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string Type;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct PidRate { public uint Pid; public double Rate; }
    [DllImport("MonitorBridge", CallingConvention = CallingConvention.Cdecl)] private static extern IntPtr sys_open();
    [DllImport("MonitorBridge", CallingConvention = CallingConvention.Cdecl)] private static extern void sys_read(IntPtr usage, out NativeView view);
    [DllImport("MonitorBridge", CallingConvention = CallingConvention.Cdecl)] private static extern void sys_close(IntPtr usage);
    private IntPtr handle = sys_open();

    public SystemReading Read()
    {
        if (handle == IntPtr.Zero) return new();
        sys_read(handle, out var v);
        var result = new SystemReading {
            Utc = v.Utc, GpuStart = v.Start, GpuEnd = v.End, Capacity = v.Capacity, Available = v.Available,
            Cpu = v.Cpu >= 0 ? v.Cpu : null, Gpu = v.Gpu >= 0 ? v.Gpu : null, GpuStatus = v.Status,
            Engine = $"LUID {v.High:x8}:{v.Low:x8} · 물리 {v.Physical} · 엔진 {v.Engine} ({v.Type})"
        };
        int size = Marshal.SizeOf<PidRate>();
        for (int i = 0; i < v.Count; ++i) {
            var p = Marshal.PtrToStructure<PidRate>(IntPtr.Add(v.Pids, checked(i * size)));
            result.GpuProcesses.Add(p.Pid, p.Rate);
        }
        return result;
    }
    public void Dispose() { if (handle != IntPtr.Zero) { sys_close(handle); handle = IntPtr.Zero; } }
}
