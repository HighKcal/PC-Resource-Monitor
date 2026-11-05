/* Measurement fixture: creates only its own ordinary, non-topmost window.
 * Does not inspect, move, message, automate, or change any other window.
 * Normal ShowWindow activation is subject to the operating system's rules. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static LRESULT CALLBACK anchor_proc(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
    switch(message) {
    case WM_PAINT: {
        PAINTSTRUCT paint; RECT client; HDC dc=BeginPaint(window,&paint);
        GetClientRect(window,&client); SetBkMode(dc,TRANSPARENT);
        DrawTextW(dc,L"Local measurement / closes automatically",-1,&client,
            DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(window,&paint); return 0;
    }
    case WM_TIMER:
        if(wp==1) { KillTimer(window,1); DestroyWindow(window); return 0; }
        break;
    case WM_DESTROY:
        KillTimer(window,1); PostQuitMessage(0); return 0;
    default:
        break;
    }
    return DefWindowProcW(window,message,wp,lp);
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show)
{
    WNDCLASSW klass={0}; HWND window; MSG message; BOOL result;
    (void)previous; (void)command; (void)show;
    klass.lpfnWndProc=anchor_proc; klass.hInstance=instance;
    klass.lpszClassName=L"LocalMeasurementFocusAnchor";
    klass.hCursor=LoadCursorW(NULL,IDC_ARROW);
    klass.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassW(&klass)) return 1;
    window=CreateWindowExW(0,klass.lpszClassName,L"Local measurement",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,20,20,320,90,NULL,NULL,instance,NULL);
    if(!window) return 1;
    if(!SetTimer(window,1,120000,NULL)) { DestroyWindow(window); return 1; }
    ShowWindow(window,SW_SHOWNORMAL); UpdateWindow(window);
    while((result=GetMessageW(&message,NULL,0,0))>0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return result<0?1:0;
}
