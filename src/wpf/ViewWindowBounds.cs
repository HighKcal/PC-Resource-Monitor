using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;

namespace PcMonitor;

// 현재 모니터의 작업 영역에 창 크기를 맞춘다. InitializeWorkArea가 HWND 메시지와
// 이동을 연결하고, FitWorkArea는 물리 픽셀을 WPF DIP로 바꿔 크기·위치를 제한한다.
public partial class MainWindow
{
    private IntPtr _windowHandle;
    private IntPtr _boundsMonitor;
    private bool _boundsRefreshQueued;
    private bool _fittingBounds;
    private HwndSource? _windowSource;

    private void InitializeWorkArea()
    {
        SourceInitialized += (_, _) => {
            _windowHandle = new WindowInteropHelper(this).Handle;
            _windowSource = HwndSource.FromHwnd(_windowHandle);
            _windowSource?.AddHook(WorkAreaWindowMessage);
            FitWorkArea();
        };
        LocationChanged += (_, _) => {
            if (_windowHandle != IntPtr.Zero && MonitorFromWindow(_windowHandle, 2) != _boundsMonitor) QueueWorkAreaRefresh();
        };
        Closed += (_, _) => { _windowSource?.RemoveHook(WorkAreaWindowMessage); _windowSource = null; };
    }

    private IntPtr WorkAreaWindowMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        // WPF가 WM_DPICHANGED의 제안 영역을 먼저 적용하게 한 뒤, 해당 모니터의
        // 새 작업 영역과 HWND의 새 DPI를 사용해 창을 맞춘다.
        if (message is 0x02E0 or 0x007E or 0x001A) QueueWorkAreaRefresh();
        return IntPtr.Zero;
    }

    private void QueueWorkAreaRefresh()
    {
        if (_boundsRefreshQueued || _fittingBounds || !IsLoaded) return;
        _boundsRefreshQueued = true;
        Dispatcher.BeginInvoke(() => { _boundsRefreshQueued = false; if (IsLoaded) { FitWorkArea(); ApplyLayout(); } }, DispatcherPriority.Loaded);
    }

    private void FitWorkArea()
    {
        if (_fittingBounds) return;
        _fittingBounds = true;
        try {
            if (_windowHandle == IntPtr.Zero) {
                ApplyWorkAreaSize(SystemParameters.WorkArea.Size); return;
            }
            var monitor = MonitorFromWindow(_windowHandle, 2);
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (monitor == IntPtr.Zero || !GetMonitorInfo(monitor, ref info)) return;
            _boundsMonitor = monitor;
            var dpi = GetDpiForWindow(_windowHandle);
            double scale = dpi > 0 ? dpi / 96d : 1;
            int width = Math.Max(1, info.Work.Right - info.Work.Left), height = Math.Max(1, info.Work.Bottom - info.Work.Top);
            ApplyWorkAreaSize(new Size(width / scale, height / scale));
            if (WindowState == WindowState.Normal && GetWindowRect(_windowHandle, out var bounds)) {
                int actualWidth = Math.Min(width, bounds.Right - bounds.Left), actualHeight = Math.Min(height, bounds.Bottom - bounds.Top);
                int left = Math.Clamp(bounds.Left, info.Work.Left, info.Work.Right - actualWidth);
                int top = Math.Clamp(bounds.Top, info.Work.Top, info.Work.Bottom - actualHeight);
                if (left != bounds.Left || top != bounds.Top || actualWidth != bounds.Right - bounds.Left || actualHeight != bounds.Bottom - bounds.Top)
                    SetWindowPos(_windowHandle, IntPtr.Zero, left, top, actualWidth, actualHeight, 0x0010 | 0x0004);
            }
        } finally { _fittingBounds = false; }
    }

    // 모니터 조회와 분리하여 작은 작업 영역의 반례에도 같은 WPF 크기 제한을
    // 독립적으로 적용할 수 있게 한다.
    private void ApplyWorkAreaSize(Size area)
    {
        double width = Math.Max(1, area.Width), height = Math.Max(1, area.Height);
        MinWidth = Math.Min(660, width); MinHeight = Math.Min(520, height);
        MaxWidth = width; MaxHeight = height;
        if (WindowState == WindowState.Normal) {
            Width = Math.Min(double.IsFinite(Width) ? Width : ActualWidth, width);
            Height = Math.Min(double.IsFinite(Height) ? Height : ActualHeight, height);
        }
    }

    [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] private struct MonitorInfo { public int Size; public NativeRect Monitor, Work; public uint Flags; }
    [DllImport("user32.dll")] private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetWindowRect(IntPtr hwnd, out NativeRect rect);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
}
