/* 사용자가 선택한 공식 URL을 데스크톱 Explorer 자동화 객체에 열도록 요청한다.
 * 메시지를 처리하는 STA worker 전용이다. WPF는 별도 STA Dispatcher에서 호출하며
 * Task.Run의 MTA에서는 RPC_E_CHANGED_MODE로 실패한다. 일반 권한 Explorer를 확인하지 못하면 실패한다. 관리자 권한의
 * ShellExecute/CreateProcess로 우회하지 않는다. URL 허용 여부는 호출부가 먼저 확인한다. */
#ifndef MONITOR_NATIVE_PROGRAM_INFO_BROWSER_H
#define MONITOR_NATIVE_PROGRAM_INFO_BROWSER_H
#include <ole2.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <servprov.h>
#include <shobjidl.h>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"oleaut32.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"advapi32.lib")
/* launch=0은 COM 연결 가능 여부만 확인하고, 1은 URL 열기까지 요청한다.
 * 부분 실패도 확보한 COM 참조·BSTR·핸들을 이 함수가 해제하고 HRESULT를 돌려준다. */
static HRESULT program_info_browser_dispatch(const char *url,int launch)
{
    DWORD pid=0,bytes=0;
    HANDLE process=NULL,token=NULL;
    TOKEN_ELEVATION elevation={0};
    HRESULT hr=E_ACCESSDENIED,initialized;
    IShellWindows *windows=NULL; IDispatch *desktop=NULL,*background=NULL,*application=NULL;
    IServiceProvider *services=NULL; IShellBrowser *browser=NULL; IShellView *view=NULL;
    IShellFolderViewDual *folder=NULL; IShellDispatch2 *shell=NULL;
    VARIANT empty,args,directory,operation,show;
    BSTR file=NULL;
    long desktop_window=0;
    wchar_t wide[2048]; char origin[256];
    if(!program_research_origin(url,origin,sizeof(origin)) || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,url,-1,wide,(int)_countof(wide))) return E_INVALIDARG;
    GetWindowThreadProcessId(GetShellWindow(),&pid);
    if(!pid) return E_FAIL;
    process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process && OpenProcessToken(process,TOKEN_QUERY,&token) &&
        GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&bytes) && !elevation.TokenIsElevated) hr=S_OK;
    if(token) CloseHandle(token); if(process) CloseHandle(process);
    if(FAILED(hr)) return hr;
    initialized=CoInitializeEx(NULL,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    if(FAILED(initialized)) return initialized;
    VariantInit(&empty); VariantInit(&args); VariantInit(&directory); VariantInit(&operation); VariantInit(&show);
    hr=CoCreateInstance(&CLSID_ShellWindows,NULL,CLSCTX_LOCAL_SERVER,&IID_IShellWindows,(void **)&windows);
    if(SUCCEEDED(hr)) hr=windows->lpVtbl->FindWindowSW(windows,&empty,&empty,SWC_DESKTOP,&desktop_window,SWFO_NEEDDISPATCH,&desktop);
    if(SUCCEEDED(hr) && desktop) hr=desktop->lpVtbl->QueryInterface(desktop,&IID_IServiceProvider,(void **)&services); else if(SUCCEEDED(hr)) hr=E_FAIL;
    if(SUCCEEDED(hr)) hr=services->lpVtbl->QueryService(services,&SID_STopLevelBrowser,&IID_IShellBrowser,(void **)&browser);
    if(SUCCEEDED(hr)) hr=browser->lpVtbl->QueryActiveShellView(browser,&view);
    if(SUCCEEDED(hr)) hr=view->lpVtbl->GetItemObject(view,SVGIO_BACKGROUND,&IID_IDispatch,(void **)&background);
    if(SUCCEEDED(hr)) hr=background->lpVtbl->QueryInterface(background,&IID_IShellFolderViewDual,(void **)&folder);
    if(SUCCEEDED(hr)) hr=folder->lpVtbl->get_Application(folder,&application);
    if(SUCCEEDED(hr)) hr=application->lpVtbl->QueryInterface(application,&IID_IShellDispatch2,(void **)&shell);
    if(SUCCEEDED(hr) && launch) {
        file=SysAllocString(wide); operation.vt=VT_BSTR; operation.bstrVal=SysAllocString(L"open");
        show.vt=VT_I4; show.lVal=SW_SHOWNORMAL;
        if(!file || !operation.bstrVal) hr=E_OUTOFMEMORY;
        else hr=shell->lpVtbl->ShellExecute(shell,file,args,directory,operation,show);
    }
    SysFreeString(file); VariantClear(&operation);
    if(shell) shell->lpVtbl->Release(shell);
    if(application) application->lpVtbl->Release(application);
    if(folder) folder->lpVtbl->Release(folder);
    if(background) background->lpVtbl->Release(background);
    if(view) view->lpVtbl->Release(view);
    if(browser) browser->lpVtbl->Release(browser);
    if(services) services->lpVtbl->Release(services);
    if(desktop) desktop->lpVtbl->Release(desktop);
    if(windows) windows->lpVtbl->Release(windows);
    CoUninitialize(); return hr;
}
static HRESULT program_info_browser_open(const char *url) { return program_info_browser_dispatch(url,1); }
#endif
