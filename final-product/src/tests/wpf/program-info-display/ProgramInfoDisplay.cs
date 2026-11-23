// 해시로 확인한 제품 assembly의 실제 프로그램 알아보기 컨트롤을 검증한다.
// 기본 모드는 명시적 메모리 fixture, --production-native는 실제 자기 프로세스의 로컬 근거다.
// 외부 조회·번역·브라우저·클립보드는 실행하지 않는다. Main이 결과 저장 후 창을 닫는다.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using PcMonitor;

internal static class ProgramInfoDisplay
{
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] static extern nint pc_test_fixture();
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] static extern void pc_test_research(nint value);
    [DllImport("MonitorBridge.dll",CallingConvention=CallingConvention.Cdecl)] static extern void pc_test_expire(nint value);
    static readonly BindingFlags Private=BindingFlags.Instance|BindingFlags.NonPublic|BindingFlags.DeclaredOnly;
    static readonly List<object> Checks=new();
    static ProgramInfoWindow Dialog=null!;
    static string Output="";
    static T Control<T>(string name) where T:FrameworkElement=>(T)Dialog.FindName(name);
    static object? Field(string name)=>typeof(ProgramInfoWindow).GetField(name,Private)!.GetValue(Dialog);
    static void Set(string name,object? value)=>typeof(ProgramInfoWindow).GetField(name,Private)!.SetValue(Dialog,value);
    static object? Call(string name,params object[] args)=>typeof(ProgramInfoWindow).GetMethod(name,Private)!.Invoke(Dialog,args);
    static void Click(string name)=>Control<Button>(name).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
    static string Hash(string path)=>Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    static void Check(bool ok,string name,object? detail=null)
    {
        Checks.Add(new {name,passed=ok,detail});
        if(!ok)throw new Exception(name);
    }
    static async Task Idle(Func<bool>? until=null)
    {
        var watch=Stopwatch.StartNew();
        do {await Task.Delay(30);Dialog.UpdateLayout();if(watch.ElapsedMilliseconds>10000)throw new Exception("UI timeout");}
        while(until!=null&&!until());
    }
    static bool Inside(FrameworkElement element)
    {
        var origin=element.TransformToAncestor(Dialog).Transform(new Point());
        return element.IsVisible && origin.Y>=0 && origin.Y+element.ActualHeight<=Dialog.ActualHeight && origin.X>=0 && origin.X+element.ActualWidth<=Dialog.ActualWidth;
    }
    static IEnumerable<DependencyObject> Tree(DependencyObject root)
    {
        yield return root;
        for(int i=0;i<VisualTreeHelper.GetChildrenCount(root);++i)
            foreach(var child in Tree(VisualTreeHelper.GetChild(root,i)))yield return child;
    }
    static RenderTargetBitmap Render(FrameworkElement element)
    {
        var bitmap=new RenderTargetBitmap((int)Math.Ceiling(element.ActualWidth),(int)Math.Ceiling(element.ActualHeight),96,96,PixelFormats.Pbgra32);
        bitmap.Render(element);return bitmap;
    }
    static void Capture(string name,Popup? popup=null)
    {
        Dialog.UpdateLayout();
        var visual=new DrawingVisual();
        double minX=0,minY=0,maxX=Dialog.ActualWidth,maxY=Dialog.ActualHeight;
        Point popupAt=new();FrameworkElement? child=popup?.Child as FrameworkElement;
        if(child!=null&&popup!.IsOpen)
        {
            child.UpdateLayout();var a=Dialog.PointToScreen(new Point());var b=child.PointToScreen(new Point());
            var dpi=VisualTreeHelper.GetDpi(Dialog);popupAt=new((b.X-a.X)/dpi.DpiScaleX,(b.Y-a.Y)/dpi.DpiScaleY);
            minX=Math.Min(0,popupAt.X);minY=Math.Min(0,popupAt.Y);maxX=Math.Max(maxX,popupAt.X+child.ActualWidth);maxY=Math.Max(maxY,popupAt.Y+child.ActualHeight);
        }
        using(var drawing=visual.RenderOpen())
        {
            drawing.DrawRectangle(Brushes.White,null,new Rect(0,0,maxX-minX,maxY-minY));
            drawing.DrawImage(Render(Dialog),new Rect(-minX,-minY,Dialog.ActualWidth,Dialog.ActualHeight));
            if(child!=null&&popup!.IsOpen)drawing.DrawImage(Render(child),new Rect(popupAt.X-minX,popupAt.Y-minY,child.ActualWidth,child.ActualHeight));
        }
        var bitmap=new RenderTargetBitmap((int)Math.Ceiling(maxX-minX),(int)Math.Ceiling(maxY-minY),96,96,PixelFormats.Pbgra32);bitmap.Render(visual);
        var encoder=new PngBitmapEncoder();encoder.Frames.Add(BitmapFrame.Create(bitmap));using var file=File.Create(Path.Combine(Output,name+".png"));encoder.Save(file);
    }
    static string ColorOf(Brush brush)=>brush is SolidColorBrush solid?solid.Color.ToString():brush.ToString();
    [STAThread] static int Main(string[] args)
    {
        // 과거 결함 재현 옵션을 현행 기본 회귀로 잘못 실행하지 않는다.
        if(args.Length<2||args.Length>3||(args.Length==3&&args[2]!="--production-native"))
        {
            Console.Error.WriteLine("Usage: ProgramInfoDisplay <output-directory> <managed-sha256> [--production-native]");
            return 2;
        }
        Output=Path.GetFullPath(args[0]);Directory.CreateDirectory(Output);
        string expected=args[1];var assembly=typeof(ProgramInfoWindow).Assembly;
        bool productionNative=args.Length==3;
        Check(Hash(assembly.Location)==expected,"loaded product assembly hash",Hash(assembly.Location));
        var app=new Application {ShutdownMode=ShutdownMode.OnExplicitShutdown};int result=1;
        app.Startup+=async(_,_)=>
        {
            var owner=new Window {Title="Program info display fixture owner",Width=200,Height=100,ShowActivated=false,ShowInTaskbar=false};
            Exception? failure=null;
            try
            {
                owner.Show();owner.Hide();
                using var process=Process.GetCurrentProcess();
                var target=new ProgramInfoTarget(7,(uint)process.Id,(ulong)process.StartTime.ToUniversalTime().ToFileTimeUtc(),process.MainModule!.FileName,"라온 렌더러 · 합성 검수");
                Dialog=new ProgramInfoWindow(owner,target,()=>target) {ShowInTaskbar=false,ShowActivated=false};
                Dialog.Show();await Idle(()=>!(bool)Field("_busy")!&&Field("_context")!=null);
                if(productionNative)
                {
                    Check(!(bool)Field("_invalid")!&&Control<TextBlock>("StatusText").Text.Contains("확인 완료"),"production native verified own process");
                    Check(Control<TextBox>("BodyText").Text.Contains("PID+생성시각")&&Control<TextBox>("BodyText").Text.Contains("서명"),"production local identity and signature evidence rendered");
                    Check(Control<Button>("ApproveButton").Visibility==Visibility.Collapsed,"production never grants automatic consent");
                    Control<ScrollViewer>("DialogScroll").ScrollToEnd();await Idle();
                    Check(Inside(Control<Button>("RefreshButton"))&&Inside(Control<TextBlock>("StatusText")),"production top status remains pinned while scrolled");
                    result=0;return;
                }
                ((IDisposable)Field("_context")!).Dispose();
                nint fixture=pc_test_fixture();Check(fixture!=0,"synthetic evidence fixture allocated");
                var handleType=assembly.GetType("PcMonitor.ProgramInfoHandle",true)!;
                object handle=Activator.CreateInstance(handleType,BindingFlags.Instance|BindingFlags.NonPublic,null,new object[]{fixture},null)!;
                Set("_context",handle);Set("_invalid",false);pc_test_research(fixture);Call("Render");Call("SetEnabled");
                Control<TextBlock>("StatusText").Text="현재 개별 프로세스 확인 완료 · 목적 선택은 선택 사항입니다. (합성 근거)";
                await Idle(); // 시험 창은 자동 활성화하지 않고 이 창의 컨트롤에만 포커스를 준다.
                Check(Control<TextBlock>("StateBadge").Text=="확인 완료","ready status badge");
                Check(Inside(Control<Button>("RefreshButton"))&&Inside(Control<TextBlock>("StatusText")),"top refresh and reason visible");
                var radio=Control<RadioButton>("ManualPurpose");radio.IsChecked=true;Keyboard.Focus(radio);await Idle();
                Check(radio.IsKeyboardFocused,"radio keyboard focus belongs to owned window");
                Check(ColorOf(radio.Foreground)=="#FF172D2C","radio dark readable foreground");
                var mark=(FrameworkElement)radio.Template.FindName("RadioMark",radio);var focus=(FrameworkElement)radio.Template.FindName("RadioFocus",radio);
                Check(mark.Visibility==Visibility.Visible&&focus.Visibility==Visibility.Visible,"radio checked and focus visuals");
                Control<TextBox>("BodyText").Text="합성 화면 검수 자료\n\n"+Control<TextBox>("BodyText").Text;
                Capture("normal-radio-focus");
                Set("_busy",true);Call("SetEnabled");await Idle();
                var disabledRadio=(Border)radio.Template.FindName("RadioSurface",radio);
                Check(!radio.IsEnabled&&!Control<Button>("RefreshButton").IsEnabled&&ColorOf(radio.Foreground)=="#FF74847D"&&ColorOf(disabledRadio.Background)=="#FFEDF1EE","busy radio and refresh disabled visuals preserve command gate");
                var clear=Tree(Dialog).OfType<Button>().Single(b=>b.Content?.ToString()=="목적 선택 지우기");
                foreach(string detail in new[]{"이 PC에서 확인하는 중입니다.","동의한 공개 항목으로 공식 자료를 확인하는 중입니다.","동의한 공개 문장 한 개의 참고 번역을 확인하는 중입니다.","일반 권한 기본 브라우저에 문서 열기를 요청하는 중입니다."})
                {
                    Control<TextBlock>("StatusText").Text=detail;Call("SetEnabled");await Idle();
                    Check(Control<TextBlock>("StatusHeading").Text=="요청한 작업을 처리하고 있어요"&&!clear.IsEnabled,"busy neutral heading and disabled clear match operation",detail);
                }
                Control<TextBlock>("StatusText").Text="동의한 공개 항목으로 공식 자료를 확인하는 중입니다.";Call("SetEnabled");await Idle();Capture("busy-fixed");
                Set("_invalid",true);Call("SetEnabled");await Idle();
                Check((string)Control<Border>("StatusCard").Tag=="Busy"&&Control<TextBlock>("StateBadge").Text=="확인 중"&&ColorOf(Control<TextBlock>("StateBadge").Foreground)=="#FF955014","busy-invalid presentation keeps prior state and color priority");
                Set("_invalid",false);Call("SetEnabled");await Idle();
                clear.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                Check(radio.IsChecked==true,"busy handler still blocks purpose clear even if invoked directly");
                Set("_busy",false);Call("SetEnabled");await Idle();
                Check(clear.IsEnabled,"purpose clear becomes enabled when busy ends");
                clear.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                Check(Control<RadioButton>("AutoPurpose").IsChecked==false&&radio.IsChecked==false,"enabled purpose clear still clears selection");
                radio.IsChecked=true;
                Control<TextBlock>("StatusText").Text="현재 개별 프로세스 확인 완료 · 목적 선택은 선택 사항입니다. (합성 근거)";

                var scroll=Control<ScrollViewer>("DialogScroll");scroll.ScrollToEnd();await Idle();
                var sources=Control<ComboBox>("Sources");Check(sources.Items.Count>=2,"two synthetic sources available",sources.Items.Count);
                Check(sources.IsEnabled&&Control<Button>("ResearchButton").IsEnabled,"normal evidence commands enabled");
                Keyboard.Focus(sources);await Idle();
                var comboSurface=(Border)sources.Template.FindName("ComboSurface",sources);
                Check(sources.IsKeyboardFocused&&comboSurface.BorderThickness.Left==2&&ColorOf(comboSurface.BorderBrush)=="#FF147865","combo keyboard focus border visible");
                sources.SelectedIndex=1;await Idle();
                Check(Control<TextBox>("SourceUrl").Text=="https://example.com/fixture","source selection updates readonly URL without opening");
                sources.IsDropDownOpen=true;await Idle();
                var popup=(Popup)sources.Template.FindName("PART_Popup",sources);
                Check(popup.IsOpen&&popup.Child is FrameworkElement,"actual combo popup opens");
                Check(popup.Child is Border border&&ColorOf(border.Background)=="#FFFFFFFF","popup explicit light surface");
                var selected=(ComboBoxItem)sources.ItemContainerGenerator.ContainerFromIndex(1);selected.ApplyTemplate();
                var choice=(Border)selected.Template.FindName("Choice",selected);
                Check(ColorOf(choice.Background)=="#FFCFE9DC","popup selected item distinct");
                Capture("normal-combo-expanded",popup);sources.IsDropDownOpen=false;await Idle();
                var url=Control<TextBox>("SourceUrl");Keyboard.Focus(url);url.SelectAll();await Idle();
                Check(url.IsReadOnly&&url.SelectionLength==url.Text.Length,"readonly URL selectable");
                var inputBorder=(Border)url.Template.FindName("InputBorder",url);
                Check(url.IsKeyboardFocused&&inputBorder.BorderThickness.Left==2&&ColorOf(inputBorder.BorderBrush)=="#FF147865","readonly URL focus border visible");
                Check(ColorOf(url.SelectionBrush)=="#FFBCE3D2"&&ColorOf(url.SelectionTextBrush)=="#FF172D2C","text selection explicit contrasting colors");
                var selectedText=Render(url);var pixels=new byte[selectedText.PixelWidth*selectedText.PixelHeight*4];
                selectedText.CopyPixels(pixels,selectedText.PixelWidth*4,0);int glyphPixels=0;
                for(int y=10;y<selectedText.PixelHeight-10;++y)for(int x=14;x<selectedText.PixelWidth-14;++x)
                {int p=(y*selectedText.PixelWidth+x)*4;if(pixels[p]<160&&pixels[p+1]<170&&pixels[p+2]<150)++glyphPixels;}
                Check(url.SelectionOpacity<0.5&&glyphPixels>20,"selected URL glyphs remain visible",new{url.SelectionOpacity,glyphPixels});
                Capture("normal-url-selection");

                double statusY=Control<TextBlock>("StatusText").TransformToAncestor(Dialog).Transform(new Point()).Y;
                Dialog.Width=460;Dialog.Height=440;scroll.ScrollToEnd();await Idle();
                Check(scroll.ScrollableHeight>0&&scroll.VerticalOffset>0,"narrow dialog body scrolls");
                Check(Inside(Control<Button>("RefreshButton"))&&Inside(Control<TextBlock>("StatusText")),"narrow scrolled refresh and reason remain pinned");
                Capture("narrow-scrolled");Dialog.Width=850;Dialog.Height=830;scroll.ScrollToHome();await Idle();

                Click("ResearchButton");Check(Control<Button>("ApproveButton").Visibility==Visibility.Visible,"preview presents explicit approval without executing");
                pc_test_expire(fixture);Call("Reconcile");await Idle();
                Check(Control<TextBlock>("StateBadge").Text=="다시 확인 필요"&&Control<TextBlock>("StatusText").Text.Contains("만료"),"expired badge and reason visible");
                Check(!Control<Button>("ResearchButton").IsEnabled&&!sources.IsEnabled&&!Control<Button>("OpenButton").IsEnabled,"expired external actions disabled");
                Check(Control<Button>("ApproveButton").Visibility==Visibility.Collapsed&&Control<Button>("RefreshButton").IsEnabled,"expired approval removed while refresh stays enabled");
                Check(ColorOf(Control<Button>("ResearchButton").Background)=="#FFEDF1EE","disabled command distinct surface");
                Check(ColorOf(Control<Border>("StatusCard").Background)=="#FFFFF2DD","expiry status warm surface");
                scroll.ScrollToHome();Capture("expired");
                bool copyEnabled=Control<Button>("CopyButton").IsEnabled;
                string invalidReason=Control<TextBlock>("StatusText").Text;
                Call("ShowCopyFeedback","선택한 문서 주소를 복사했습니다.");await Idle();
                string copied=Control<TextBlock>("StatusText").Text;
                Check(copied==invalidReason+Environment.NewLine+"선택한 문서 주소를 복사했습니다.","copy feedback keeps direct expiry reason without clipboard call");
                Call("Reconcile");await Idle();
                Check(Control<TextBlock>("StatusText").Text==copied,"expiry reconciliation preserves current feedback without accumulating");
                Call("ShowCopyFeedback","클립보드를 사용할 수 없습니다. 주소를 직접 복사하세요.");await Idle();
                Check(Control<TextBlock>("StatusText").Text==invalidReason+Environment.NewLine+"클립보드를 사용할 수 없습니다. 주소를 직접 복사하세요.","new copy feedback replaces prior result while retaining reason");
                Call("ShowCopyFeedback","선택한 문서 주소를 복사했습니다.");await Idle();
                Check(Control<Button>("CopyButton").IsEnabled==copyEnabled&&Inside(Control<TextBlock>("StatusText"))&&Inside(Control<Button>("RefreshButton")),"copy eligibility unchanged and expiry feedback remains pinned");
                Capture("expired-copy-feedback");
                await (Task)Call("RefreshAsync")!;
                Check(!(bool)Field("_invalid")!&&Field("_invalidReason")==null&&Field("_copyFeedback")==null&&Control<TextBlock>("StatusText").Text.Contains("확인 완료"),"successful reconfirmation clears expiry and old copy feedback");
                result=0;
            }
            catch(Exception error){failure=error;Console.WriteLine(error);}
            finally
            {
                File.WriteAllText(Path.Combine(Output,"results.json"),JsonSerializer.Serialize(new {passed=result==0,syntheticEvidence=!productionNative,productionNative,productManagedHash=Hash(assembly.Location),nativeHash=Hash(Path.Combine(AppContext.BaseDirectory,"MonitorBridge.dll")),externalRequests=false,clipboardWrites=false,checks=Checks,error=failure?.ToString(),capture="Actual WPF visual render. Open popup is rendered from its own live visual at its actual screen offset; no foreign screen pixels."},new JsonSerializerOptions{WriteIndented=true}));
                Dialog?.Close();owner.Close();app.Shutdown(result);
            }
        };app.Run();return result;
    }
}
