/* monitor_gui.c의 App과 컨트롤을 사용하는 비공개 Win32 표시 구현 조각이다.
 * 수집·저장·판정 상태를 별도로 소유하지 않으며 독립 라이브러리 API가 아니다. */
#ifndef MONITOR_NATIVE_UI_H
#define MONITOR_NATIVE_UI_H
#define UI_INK RGB(28,46,57)
#define UI_TEAL RGB(0,111,112)
#define UI_BACKGROUND RGB(243,247,248)
#define UI_MUTED RGB(83,103,114)
static int ui_px(int dip) { return MulDiv(dip,(int)(app.dpi?app.dpi:96),96); }
static void ui_enable_dpi(void)
{
    HMODULE user=GetModuleHandleW(L"user32.dll"),shell;
    union { FARPROC raw; BOOL (WINAPI *context)(HANDLE); } aware;
    union { FARPROC raw; HRESULT (WINAPI *process)(int); } legacy;
    aware.raw=GetProcAddress(user,"SetProcessDpiAwarenessContext");
    if(aware.raw && aware.context((HANDLE)(INT_PTR)-4)) return;
    shell=LoadLibraryW(L"shcore.dll");
    if(shell) {
        HRESULT result;
        legacy.raw=GetProcAddress(shell,"SetProcessDpiAwareness");
        result=legacy.raw?legacy.process(2):E_NOTIMPL;
        FreeLibrary(shell);
        if(SUCCEEDED(result) || result==E_ACCESSDENIED) return;
    }
    SetProcessDPIAware();
}
static UINT ui_window_dpi(HWND window)
{
    union { FARPROC raw; UINT (WINAPI *get)(HWND); } query;
    HDC dc; UINT dpi;
    if(app.ui_scale) return MulDiv(96,(int)app.ui_scale,100);
    query.raw=GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow");
    if(query.raw && window) return query.get(window);
    dc=GetDC(window); dpi=dc?(UINT)GetDeviceCaps(dc,LOGPIXELSY):96;
    if(dc) ReleaseDC(window,dc);
    return dpi?dpi:96;
}
static BOOL CALLBACK ui_child_font(HWND child,LPARAM unused)
{
    int id=GetDlgCtrlID(child);
    (void)unused;
    SendMessageW(child,WM_SETFONT,(WPARAM)(id==ID_TITLE?app.title_font:
        id==ID_LIST_TITLE || id==ID_DETAIL_TITLE?app.heading_font:app.body_font),TRUE);
    return TRUE;
}
static void ui_fonts(void)
{
    HFONT body=app.body_font,title=app.title_font,heading=app.heading_font;
    HIMAGELIST images=app.row_images;
    app.body_font=CreateFontW(-MulDiv(11,(int)app.dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Malgun Gothic");
    app.heading_font=CreateFontW(-MulDiv(11,(int)app.dpi,72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Malgun Gothic");
    app.title_font=CreateFontW(-MulDiv(19,(int)app.dpi,72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Malgun Gothic");
    EnumChildWindows(app.window,ui_child_font,0);
    app.row_images=ImageList_Create(1,ui_px(36),ILC_COLOR32,1,1);
    ListView_SetImageList(app.list,app.row_images,LVSIL_SMALL);
    if(images) ImageList_Destroy(images);
    if(body) DeleteObject(body); if(title) DeleteObject(title); if(heading) DeleteObject(heading);
    SendMessageW(app.detail,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(ui_px(12),ui_px(12)));
}
static void ui_columns(int list_width)
{
    const int fixed[]={0,60,76,88,88,96};
    int name=app.name_width_dip?ui_px(app.name_width_dip):list_width-ui_px(408)-GetSystemMetrics(SM_CXVSCROLL)-2;
    unsigned i;
    if(name<ui_px(140)) name=ui_px(140);
    app.setting_columns=1;
    ListView_SetColumnWidth(app.list,0,name);
    for(i=1;i<sizeof(fixed)/sizeof(fixed[0]);++i) ListView_SetColumnWidth(app.list,(int)i,ui_px(fixed[i]));
    app.setting_columns=0;
}
static void ui_dispose(void)
{
    if(app.body_font) DeleteObject(app.body_font);
    if(app.heading_font) DeleteObject(app.heading_font);
    if(app.title_font) DeleteObject(app.title_font);
    if(app.row_images) { ListView_SetImageList(app.list,NULL,LVSIL_SMALL); ImageList_Destroy(app.row_images); }
    if(app.background_brush) DeleteObject(app.background_brush);
    if(app.panel_brush) DeleteObject(app.panel_brush);
    app.body_font=app.heading_font=app.title_font=NULL;
    app.row_images=NULL; app.background_brush=app.panel_brush=NULL;
}
#endif
