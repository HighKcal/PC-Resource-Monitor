using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace PcMonitor;

public enum TrayAction { None, ShowWindow, Toggle, Exit, Click, Retire }
public enum TrayRequestResult { Requested, Unavailable, Quiet, Rejected }

/// <summary>기존 native 알림 Shell을 WPF HWND에 연결하며 모든 메서드는 UI Dispatcher에서
/// 호출한다. 정책을 소유한 engine worker는 Request 직전에 dispatch하고, 다른 정책 변경
/// 명령을 처리하기 전에 결과를 정확히 한 번 보고한다.</summary>
public sealed class TrayHost : IDisposable
{
    public const int CallbackMessage = 0x8000 + 0x172;
    private readonly Dispatcher _dispatcher;
    private readonly HwndSource _source;
    private readonly Window _owner;
    private nint _handle;
    public bool Enabled { get; set; } = true;
    public event Action<TrayAction, ulong>? ActionReceived;
    public bool Ready { get { VerifyAccess(); return _handle != 0 && Native.pc_tray_ready(_handle) != 0; } }

    public TrayHost(Window owner)
    {
        _owner = owner;
        _dispatcher = owner.Dispatcher;
        _dispatcher.VerifyAccess();
        nint hwnd = new WindowInteropHelper(owner).EnsureHandle();
        _source = HwndSource.FromHwnd(hwnd) ?? throw new InvalidOperationException("WPF HWND를 준비하지 못했습니다.");
        _source.AddHook(WindowMessage);
        _handle = Native.pc_tray_create(hwnd, CallbackMessage);
        owner.Closed += OwnerClosed;
    }
    private void VerifyAccess() => _dispatcher.VerifyAccess();
    public TrayRequestResult Request(in NativeNotification notification)
    {
        VerifyAccess();
        if (_handle == 0 || !Enabled) return TrayRequestResult.Unavailable;
        var value = notification;
        return (TrayRequestResult)Native.pc_tray_request(_handle, ref value, (ulong)Environment.TickCount64);
    }
    /// <summary>최초 tray 등록 실패 때도 매 UI tick에서 호출한다. 반환된 종료 event ID는
    /// engine worker에 한 번만 전달한다. HWND 콜백과 달리 ActionReceived를 발생시키지 않는다.</summary>
    public ulong Tick()
    {
        VerifyAccess();
        return _handle != 0 ? Native.pc_tray_tick(_handle, (ulong)Environment.TickCount64) : 0;
    }
    public void Cancel(ulong eventId)
    {
        VerifyAccess();
        if (_handle != 0) Native.pc_tray_cancel(_handle, eventId);
    }
    private nint WindowMessage(nint hwnd, int message, nint wParam, nint lParam, ref bool handled)
    {
        if (_handle == 0) return 0;
        if (Native.pc_tray_message(_handle, (uint)message, (nuint)wParam, lParam, Enabled ? 1 : 0,
            out int action, out ulong id) == 0) return 0;
        handled = true;
        // native Shell IPC와 재진입 메뉴 밖에서 처리하도록 게시한다. 정책 dispatcher가
        // 이 콜백을 결과 보고·일시정지·피드백·종료와 직렬화한다.
        if (action != 0)
            _dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
            {
                if (_handle != 0) ActionReceived?.Invoke((TrayAction)action, id);
            }));
        return 0;
    }
    private void OwnerClosed(object? sender, EventArgs e) => Dispose();
    public void Dispose()
    {
        VerifyAccess();
        if (_handle == 0) return;
        nint handle = _handle;
        _handle = 0;
        _owner.Closed -= OwnerClosed;
        _source.RemoveHook(WindowMessage);
        Native.pc_tray_destroy(handle);
        GC.SuppressFinalize(this);
    }
    private static class Native
    {
        private const string Library = "MonitorBridge.dll";
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern nint pc_tray_create(nint owner, uint callbackMessage);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pc_tray_ready(nint context);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pc_tray_request(nint context, ref NativeNotification notification, ulong nowMs);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int pc_tray_message(nint context, uint message, nuint wp, nint lp, int enabled, out int action, out ulong eventId);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern ulong pc_tray_tick(nint context, ulong nowMs);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pc_tray_cancel(nint context, ulong eventId);
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        internal static extern void pc_tray_destroy(nint context);
    }
}
