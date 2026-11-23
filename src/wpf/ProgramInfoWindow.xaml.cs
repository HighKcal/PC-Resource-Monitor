using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using Microsoft.Win32.SafeHandles;

namespace PcMonitor;

public sealed record ProgramInfoTarget(uint Session, uint Pid, ulong Creation,
    string Path, string Name, bool Synthetic = false)
{
    internal bool SameInstance(ProgramInfoTarget? other) => other is not null &&
        Session == other.Session && Pid == other.Pid && Creation == other.Creation &&
        Synthetic == other.Synthetic && string.Equals(Path, other.Path, StringComparison.Ordinal);
}

/// <summary>로컬 근거와 동의한 공개 정보의 전송 경계는 native provider가 소유한다.
/// 이 창은 표시·현재 선택·사용 목적을 관리한다. RefreshAsync/ExecuteAsync에서 비동기
/// 결과의 generation과 현재 인스턴스를 확인한 뒤 Render로 표시한다.</summary>
public partial class ProgramInfoWindow : Window
{
    private readonly Func<ProgramInfoTarget?> _currentTarget;
    private readonly DispatcherTimer _reconcileTimer;
    private readonly List<ProgramSource> _sources = new();
    private ProgramInfoTarget _target;
    private ProgramInfoHandle? _context;
    private EventWaitHandle? _cancel;
    private bool _busy, _closed, _invalid, _details, _ready;
    private long _generation;
    private int _previewOperation = -1;
    private string? _invalidReason, _copyFeedback;
    private uint Intent => AutoPurpose.IsChecked == true ? 1u : ManualPurpose.IsChecked == true ? 2u : 0u;

    public ProgramInfoWindow(Window owner, ProgramInfoTarget target, Func<ProgramInfoTarget?> currentTarget)
    {
        InitializeComponent();
        Owner = owner;
        _target = target;
        _currentTarget = currentTarget;
        _reconcileTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(500), DispatcherPriority.Background,
            (_, _) => Reconcile(), Dispatcher);
        Loaded += async (_, _) => await RefreshAsync();
        Closed += (_, _) =>
        {
            _closed = true;
            CancelPending();
            _reconcileTimer.Stop();
            _context?.Dispose();
            _context = null;
        };
        _ready = true;
        SetEnabled();
    }

    /// <summary>generation을 바꿔 늦은 worker 응답의 수락을 막고 진행 중 작업에 취소를 알린다.</summary>
    public void CancelPending()
    {
        Dispatcher.VerifyAccess();
        ++_generation;
        _cancel?.Set();
        if (_busy && !_closed && !_invalid)
        {
            _invalid = true;
            _previewOperation = -1;
            ShowInvalidStatus("요청을 취소했습니다. 현재 목록에서 새로고침하세요.");
        }
    }

    private ProgramInfoTarget? CurrentTarget()
    {
        try { return _currentTarget(); }
        catch (InvalidOperationException) { return null; }
    }

    // UI 타이머와 비동기 완료 지점에서 선택·세션을 확인한다. native 작업 중에는 같은
    // handle을 조회하지 않고, 불일치/만료를 발견하면 이전 동의와 결과 사용을 막는다.
    private bool Reconcile()
    {
        if (_closed) return false;
        if (!_target.SameInstance(CurrentTarget()))
        {
            if (!_invalid)
            {
                _invalid = true;
                CancelPending();
                _previewOperation = -1;
                BodyText.Text = "선택·수집 세션 또는 현재 관측이 바뀌었습니다. 이전 결과를 다른 프로세스에 연결하지 않습니다.\r\n현재 목록에서 개별 프로세스를 선택하고 새로고침하세요.";
                ShowInvalidStatus("현재 선택과의 연결 미확인 · 이전 동의는 끝났습니다.");
                SetEnabled();
            }
            return false;
        }
        if (!_busy && _context is not null && !_context.IsClosed && !ProgramInfoNative.Current(_context))
        {
            _invalid = true;
            _previewOperation = -1;
            ShowInvalidStatus("프로세스 종료·신원 미확인 또는 60초 근거 만료 · 새로고침하세요.");
            SetEnabled();
            return false;
        }
        return !_invalid;
    }

    // 로컬 수집만 Task.Run에 맡기고 await 이후 UI에서 결과를 수락한다. 세대나 현재
    // 대상이 달라졌으면 result를 finally에서 해제하고, 수락했으면 _context로 소유권을 옮긴다.
    private async Task RefreshAsync()
    {
        if (_busy || _closed) return;
        var current = CurrentTarget();
        if (current is null)
        {
            StatusText.Text = "정상 수집 중인 현재 개별 프로세스를 먼저 선택하세요.";
            return;
        }
        CancelPending();
        _target = current;
        _invalid = false;
        _copyFeedback = null;
        _previewOperation = -1;
        _context?.Dispose();
        _context = null;
        long generation = _generation;
        _busy = true;
        TargetLabel.Text = $"{_target.Name} · PID {_target.Pid}";
        StatusText.Text = "이 PC에서 확인하는 중 · 수집 화면은 계속 사용할 수 있습니다.";
        BodyText.Text = "실행 중인 인스턴스·정확한 경로·로컬 파일·오프라인 서명 근거를 확인합니다. 외부 조회는 보내지 않습니다.";
        SetEnabled();
        using var cancel = new EventWaitHandle(false, EventResetMode.ManualReset);
        _cancel = cancel;
        ProgramInfoHandle? result = null;
        try
        {
            var target = _target;
            result = await Task.Run(() => ProgramInfoNative.Collect(target, cancel.SafeWaitHandle));
            if (_closed || generation != _generation || !Reconcile()) return;
            _context = result;
            result = null;
            // 실패 결과도 설명할 본문은 있다. 표시 중 SourceChanged가 상태를
            // 갱신하므로 먼저 현재성을 판정해 일시적으로 Ready가 되지 않게 한다.
            bool currentEvidence = ProgramInfoNative.Current(_context);
            _invalid = !currentEvidence;
            _busy = false;
            if (currentEvidence)
            {
                _invalidReason = null;
                _copyFeedback = null;
                StatusText.Text = "현재 개별 프로세스 확인 완료 · 목적 선택은 선택 사항입니다.";
            }
            else ShowInvalidStatus("프로세스 종료·신원 미확인 또는 60초 근거 만료 · 새로고침하세요.");
            RenderResult();
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or InvalidOperationException)
        {
            if (!_closed && generation == _generation) StatusText.Text = $"프로그램 정보 조회를 시작할 수 없습니다: {ex.Message}";
        }
        finally
        {
            result?.Dispose();
            if (ReferenceEquals(_cancel, cancel)) _cancel = null;
            _busy = false;
            if (!_closed) { SetEnabled(); }
        }
    }

    private void Render()
    {
        if (_closed || _busy || _context is null || _context.IsClosed || _invalid) return;
        RenderResult();
    }

    // 수락된 정상 결과와 로컬 수집의 실패 설명을 그린다. 호출자는 먼저 현재성에
    // 따른 _invalid를 정한다. 본문이 있다는 이유로 조회 동작을 허용하지 않는다.
    private void RenderResult()
    {
        if (_context is null || _context.IsClosed) return;
        BodyText.Text = ProgramInfoNative.Text(_context, Intent, _details);
        _sources.Clear();
        uint count = ProgramInfoNative.SourceCount(_context);
        for (uint i = 0; i < count; ++i)
            _sources.Add(new ProgramSource(ProgramInfoNative.Source(_context, i, false), ProgramInfoNative.Source(_context, i, true)));
        int previous = Sources.SelectedIndex;
        Sources.ItemsSource = null;
        Sources.ItemsSource = _sources;
        Sources.DisplayMemberPath = nameof(ProgramSource.Title);
        Sources.SelectedIndex = _sources.Count > 0 ? Math.Clamp(previous, 0, _sources.Count - 1) : -1;
    }

    private void SetEnabled()
    {
        if (!_ready || _closed) return;
        // 상태 배지의 표시를 먼저 정한다. 명령 허용 여부는 아래 evidence 조건으로 따로 계산한다.
        string state = _busy ? "Busy" : _invalid ? "Invalid" : _context is not null && !_context.IsClosed ? "Ready" : "Pending";
        StatusCard.Tag = state;
        StateBadge.Tag = _invalid;
        StateBadge.Text = state switch { "Busy" => "확인 중", "Invalid" => "다시 확인 필요", "Ready" => "확인 완료", _ => "확인 전" };
        StatusHeading.Text = state switch { "Busy" => "요청한 작업을 처리하고 있어요", "Invalid" => "연결을 새로 확인하세요", "Ready" => "현재 선택과 연결된 프로그램 정보", _ => "프로그램 정보를 확인하세요" };
        bool evidence = !_busy && !_invalid && _context is not null && !_context.IsClosed;
        AutoPurpose.IsEnabled = ManualPurpose.IsEnabled = !_busy;
        DetailsButton.IsEnabled = evidence;
        ResearchButton.IsEnabled = TranslateButton.IsEnabled = evidence;
        RefreshButton.IsEnabled = !_busy;
        Sources.IsEnabled = evidence;
        OpenButton.IsEnabled = evidence && Sources.SelectedIndex >= 0;
        CopyButton.IsEnabled = !_busy && !string.IsNullOrEmpty(SourceUrl.Text);
        ApproveButton.Visibility = CancelPreviewButton.Visibility = _previewOperation >= 0 ? Visibility.Visible : Visibility.Collapsed;
        ApproveButton.IsEnabled = evidence;
    }

    private void Preview(int operation)
    {
        if (_busy || !Reconcile() || _context is null) return;
        BodyText.Text = ProgramInfoNative.Preview(_context, Intent, operation);
        _previewOperation = ProgramInfoNative.PreviewReady(_context) ? operation : -1;
        ApproveButton.Content = operation == 2 ? "이 문장 번역" : "이 내용으로 조회";
        StatusText.Text = _previewOperation >= 0 ? "보낼 값과 목적지를 확인하세요. 아직 전송하지 않았습니다." : "외부 요청을 시작하지 않았습니다.";
        BodyText.ScrollToHome();
        SetEnabled();
    }

    // Shell 자동화는 메시지를 처리하는 별도 STA에서 실행한다. Task.Run의 MTA를
    // STA로 바꾸려 하면 RPC_E_CHANGED_MODE가 나며 UI 스레드의 동기 실행도 피한다.
    private static Task<int> RunBrowserAsync(Func<int> operation)
    {
        var completion = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            var dispatcher = Dispatcher.CurrentDispatcher;
            dispatcher.BeginInvoke(new Action(() =>
            {
                try { completion.TrySetResult(operation()); }
                catch (Exception error) { completion.TrySetException(error); }
                finally { dispatcher.BeginInvokeShutdown(DispatcherPriority.Send); }
            }));
            Dispatcher.Run();
        }) { IsBackground = true, Name = "Program information browser" };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        return completion.Task;
    }

    // 승인한 조회·번역 또는 명시한 URL 열기를 하나씩 실행한다. _busy 동안 같은
    // context를 UI에서 읽지 않으며, 완료가 늦게 도착하면 generation으로 상태 문구 갱신을 막는다.
    private async Task ExecuteAsync(int operation)
    {
        if (_busy || !Reconcile() || _context is null) return;
        var context = _context;
        uint intent = Intent;
        uint source = (uint)Math.Max(Sources.SelectedIndex, 0);
        long generation = _generation;
        _busy = true;
        _previewOperation = -1;
        StatusText.Text = operation == 2 ? "동의한 공개 문장 한 개의 참고 번역을 확인하는 중입니다." :
            operation == 1 ? "일반 권한 기본 브라우저에 문서 열기를 요청하는 중입니다." : "동의한 공개 항목으로 공식 자료를 확인하는 중입니다.";
        SetEnabled();
        using var cancel = new EventWaitHandle(false, EventResetMode.ManualReset);
        _cancel = cancel;
        bool completed = false;
        try
        {
            if (operation == 1)
                await RunBrowserAsync(() => ProgramInfoNative.Execute(context, intent, operation, source, cancel.SafeWaitHandle));
            else
                await Task.Run(() => ProgramInfoNative.Execute(context, intent, operation, source, cancel.SafeWaitHandle));
            completed = true;
        }
        catch (Exception ex) when (ex is InvalidOperationException or ObjectDisposedException)
        {
            if (!_closed && generation == _generation) StatusText.Text = $"작업을 완료하지 못했습니다: {ex.Message}";
        }
        finally
        {
            if (ReferenceEquals(_cancel, cancel)) _cancel = null;
            _busy = false;
            if (!_closed)
            {
                // 작업 중에는 native handle을 읽지 않는다. 완료 후 세대·선택·근거를
                // 모두 확인한 결과만 그린다. 무효 응답은 기존 본문·복사 주소를 보존한다.
                if (Reconcile() && generation == _generation && completed)
                {
                    Render();
                    StatusText.Text = "작업 결과와 확인 한계를 아래에 표시했습니다.";
                }
                SetEnabled();
            }
        }
    }

    private void PurposeChanged(object sender, RoutedEventArgs e)
    {
        if (!_ready || _busy) return;
        if (_context is not null && !_context.IsClosed) ProgramInfoNative.ClearPreview(_context);
        _previewOperation = -1;
        Render(); SetEnabled();
    }
    private void ClearPurpose(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        AutoPurpose.IsChecked = ManualPurpose.IsChecked = false;
        PurposeChanged(sender, e);
    }
    private void ToggleDetails(object sender, RoutedEventArgs e)
    {
        _details = !_details;
        DetailsButton.Content = _details ? "− 자세한 근거" : "+ 자세한 근거";
        if (_previewOperation >= 0) CancelPreview(sender, e); else Render();
    }
    private void SourceChanged(object sender, SelectionChangedEventArgs e)
    {
        SourceUrl.Text = Sources.SelectedItem is ProgramSource source ? source.Url : "";
        SetEnabled();
    }
    private void CopyUrl(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrEmpty(SourceUrl.Text)) return;
        try { Clipboard.SetText(SourceUrl.Text); ShowCopyFeedback("선택한 문서 주소를 복사했습니다."); }
        catch (ExternalException) { ShowCopyFeedback("클립보드를 사용할 수 없습니다. 주소를 직접 복사하세요."); }
    }
    private void ShowInvalidStatus(string reason)
    {
        if (!string.Equals(_invalidReason, reason, StringComparison.Ordinal)) _copyFeedback = null;
        _invalidReason = reason;
        StatusText.Text = FormatStatus(reason, _copyFeedback);
    }
    private void ShowCopyFeedback(string feedback)
    {
        _copyFeedback = feedback;
        StatusText.Text = FormatStatus(_invalid ? _invalidReason : null, feedback);
    }
    private static string FormatStatus(string? reason, string? feedback) =>
        string.IsNullOrEmpty(reason) ? feedback ?? "" : string.IsNullOrEmpty(feedback) ? reason : reason + Environment.NewLine + feedback;
    private void PreviewResearch(object sender, RoutedEventArgs e) => Preview(0);
    private void PreviewTranslation(object sender, RoutedEventArgs e) => Preview(2);
    private async void ApprovePreview(object sender, RoutedEventArgs e) { if (_previewOperation >= 0) await ExecuteAsync(_previewOperation); }
    private void CancelPreview(object sender, RoutedEventArgs e)
    {
        if (_busy) return;
        if (_context is not null && !_context.IsClosed) ProgramInfoNative.ClearPreview(_context);
        _previewOperation = -1; Render(); SetEnabled();
        StatusText.Text = "전송 동의를 끝냈습니다. 새 조회는 다시 확인한 뒤 선택하세요.";
    }
    private async void OpenSource(object sender, RoutedEventArgs e) => await ExecuteAsync(1);
    private async void RefreshClicked(object sender, RoutedEventArgs e) => await RefreshAsync();
    private void CloseClicked(object sender, RoutedEventArgs e) => Close();
    private sealed record ProgramSource(string Title, string Url);
}

// native 결과의 해제는 이 SafeHandle에 모은다. P/Invoke에는 이 객체를 전달하고,
// 창이 수락하지 않은 수집 결과와 교체·닫기로 끝난 결과는 Dispose로 반환한다.
internal sealed class ProgramInfoHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal ProgramInfoHandle(nint value) : base(true) => SetHandle(value);
    protected override bool ReleaseHandle() { ProgramInfoNative.Destroy(handle); return true; }
}

internal static class ProgramInfoNative
{
    private const string Library = "MonitorBridge.dll";
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_collect")]
    private static extern nint CollectNative(uint pid, ulong creation, uint session, int synthetic,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path, SafeWaitHandle cancel);
    internal static ProgramInfoHandle Collect(ProgramInfoTarget target, SafeWaitHandle cancel)
    {
        nint result = CollectNative(target.Pid, target.Creation, target.Session, target.Synthetic ? 1 : 0, target.Path, cancel);
        if (result == 0) throw new InvalidOperationException("로컬 정보 메모리를 할당하지 못했습니다.");
        return new ProgramInfoHandle(result);
    }
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_destroy")]
    internal static extern void Destroy(nint context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_current")]
    private static extern int CurrentNative(ProgramInfoHandle context);
    internal static bool Current(ProgramInfoHandle context) => CurrentNative(context) != 0;
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_text")]
    private static extern nint TextNative(ProgramInfoHandle context, uint intent, int details);
    internal static string Text(ProgramInfoHandle context, uint intent, bool details) => Marshal.PtrToStringUTF8(TextNative(context, intent, details ? 1 : 0)) ?? "";
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_preview")]
    private static extern nint PreviewNative(ProgramInfoHandle context, uint intent, int operation);
    internal static string Preview(ProgramInfoHandle context, uint intent, int operation) => Marshal.PtrToStringUTF8(PreviewNative(context, intent, operation)) ?? "";
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_preview_ready")]
    private static extern int PreviewReadyNative(ProgramInfoHandle context);
    internal static bool PreviewReady(ProgramInfoHandle context) => PreviewReadyNative(context) != 0;
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_execute")]
    internal static extern int Execute(ProgramInfoHandle context, uint intent, int operation, uint source, SafeWaitHandle cancel);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_clear_preview")]
    internal static extern void ClearPreview(ProgramInfoHandle context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_source_count")]
    internal static extern uint SourceCount(ProgramInfoHandle context);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl, EntryPoint = "pc_program_source")]
    private static extern nint SourceNative(ProgramInfoHandle context, uint index, int url);
    internal static string Source(ProgramInfoHandle context, uint index, bool url) => Marshal.PtrToStringUTF8(SourceNative(context, index, url ? 1 : 0)) ?? "";
}
