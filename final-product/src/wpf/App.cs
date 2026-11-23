// WPF의 실행 진입점이다. Options.Parse가 실수집·저장 합성·화면 시안 모드를 구분하고,
// App.Main이 WindowController를 연결한다. 실제 worker 시작과 종료는 그 조율자를 읽는다.
using System;
using System.Globalization;
using System.IO;
using System.Diagnostics;
using System.Windows;

namespace PcMonitor;

internal enum InputMode { Live, PersistedSynthetic, ScreenDemo, SmallGroupsDemo, NotificationDemo }
internal sealed record Options
{
    public InputMode Mode { get; private set; }
    public int Interval { get; private set; } = 3000;
    public int Count { get; private set; }
    public string Database { get; private set; } = "";
    public string? Diagnostics { get; private set; }
    public Size? WindowSize { get; private set; }
    public int PreviousPid { get; private set; }
    public long PreviousCreation { get; private set; }
    public static Options Parse(string[] args)
    {
        var o=new Options();
        for (int i=0;i<args.Length;i++) {
            string flag=args[i];
            string Next() => ++i<args.Length ? args[i] : throw new ArgumentException($"{flag} 인수가 필요합니다.");
            int Positive(string value) => int.TryParse(value,NumberStyles.None,CultureInfo.InvariantCulture,out var n) && n>0 ? n : throw new ArgumentException($"{flag} 값은 양의 정수여야 합니다.");
            void Mode(InputMode m) { if(o.Mode!=InputMode.Live) throw new ArgumentException("한 가지 입력 모드를 선택하세요."); o.Mode=m; }
            switch(flag) {
                case "--ui-demo": Mode(InputMode.ScreenDemo); break;
                case "--ui-small-groups": Mode(InputMode.SmallGroupsDemo); break;
                case "--ui-notification-demo": Mode(InputMode.NotificationDemo); break;
                case "--synthetic": Mode(InputMode.PersistedSynthetic); break;
                case "--db": o.Database=Path.GetFullPath(Next()); break;
                case "--interval-ms": o.Interval=Positive(Next()); if(o.Interval>60000) throw new ArgumentException("간격 최대값은 60000 ms입니다."); break;
                case "--count": o.Count=Positive(Next()); break;
                case "--diagnostics": o.Diagnostics=Path.GetFullPath(Next()); break;
                case "--wait-for-instance":
                    o.PreviousPid=Positive(Next()); o.PreviousCreation=long.Parse(Next(),CultureInfo.InvariantCulture);
                    break;
                case "--ui-size":
                    var parts=Next().Split('x'); if(parts.Length!=2) throw new ArgumentException("--ui-size 960x640");
                    o.WindowSize=new Size(Positive(parts[0]),Positive(parts[1])); break;
                default: throw new ArgumentException($"알 수 없는 인수 {flag}");
            }
        }
        if(o.Mode==InputMode.PersistedSynthetic && o.Database=="") throw new ArgumentException("--synthetic에는 별도 시험 DB 경로가 필요합니다.");
        if(o.Mode==InputMode.Live && o.Database=="") o.Database=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"PC-Resource-Monitor","Wpf","history.sqlite");
        if(o.Database.StartsWith(@"\\",StringComparison.Ordinal)) throw new ArgumentException("로컬 디스크의 DB 경로를 사용하세요.");
        return o;
    }

    // 관리자 재실행으로 넘겨받은 PID와 생성시각이 모두 같을 때만 이전 인스턴스의
    // 종료를 기다린다. 30초 안에 끝나지 않으면 새 worker의 DB 개방 전에 실패한다.
    public void WaitForPrevious()
    {
        if(PreviousPid==0 || PreviousPid==Environment.ProcessId) return;
        try {
            using var process=Process.GetProcessById(PreviousPid);
            if(process.StartTime.ToUniversalTime().ToFileTimeUtc()==PreviousCreation && !process.WaitForExit(30000))
                throw new TimeoutException("이전 모니터가 DB 사용을 마치지 않았습니다.");
        } catch(ArgumentException) { } catch(InvalidOperationException) { } // 이미 종료된 프로세스는 기다리지 않는다.
    }
}

internal sealed class App : Application
{
    [STAThread]
    public static int Main(string[] args)
    {
        try {
            var options=Options.Parse(args);
            var app=new App { ShutdownMode=ShutdownMode.OnExplicitShutdown };
            var window=new MainWindow();
            if(options.WindowSize is Size size) {window.Width=size.Width; window.Height=size.Height;}
            window.Width=Math.Min(window.Width,SystemParameters.WorkArea.Width);
            window.Height=Math.Min(window.Height,SystemParameters.WorkArea.Height);
            _=new WindowController(window,options);
            return app.Run(window);
        } catch(Exception e) { MessageBox.Show(e.Message,"내 PC 자원 모니터",MessageBoxButton.OK,MessageBoxImage.Error); return 1; }
    }
}
