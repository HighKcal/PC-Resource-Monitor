/* Independent scenarios added to the prior hidden GUI suite. */
static void test_access_metadata_binding(void)
{
    ResourceDisplayBatch display={0}; ResourceDisplayInfo info={0};
    MonitorSnapshot snapshot={0}; MonitorRow row={0};
    display.items=&info; display.count=1; snapshot.rows=&row; snapshot.count=1;
    row.raw.pid=42; row.raw.measured_at_utc_100ns=999; row.raw.creation_time_utc_100ns=100;
    row.raw.valid_fields=RESOURCE_VALID_IDENTITY; info.pid=42; info.measured_at_utc_100ns=999; info.creation_time_utc_100ns=100;
    info.name_source=RESOURCE_NAME_LIST; info.snapshot_name_utf8="list.exe";
    CHECK(ui_display_metadata(&display,&snapshot,0)==&info);
    CHECK(ui_name_source(&row,&info)==RESOURCE_NAME_LIST);
    row.raw.process_name_utf8="stale-old-handle.exe";
    CHECK(!strcmp(ui_name_text(&row,&info),"list.exe"));
    CHECK(ui_name_source(&row,NULL)==RESOURCE_NAME_UNAVAILABLE);
    row.raw.valid_fields|=RESOURCE_VALID_NAME;
    CHECK(ui_name_source(&row,&info)==RESOURCE_NAME_HANDLE);
    CHECK(!strcmp(ui_name_text(&row,&info),"stale-old-handle.exe"));
    row.raw.valid_fields&=~RESOURCE_VALID_NAME;
    info.pid=43; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); info.pid=42;
    info.creation_time_utc_100ns=101; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); info.creation_time_utc_100ns=100;
    ++info.measured_at_utc_100ns; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); --info.measured_at_utc_100ns;
    display.count=2; CHECK(ui_display_metadata(&display,&snapshot,0)==NULL); display.count=1;
    CHECK(ui_display_metadata(&display,&snapshot,1)==NULL);
    row.raw.valid_fields=0; info.creation_time_utc_100ns=0;
    CHECK(ui_display_metadata(&display,&snapshot,0)==&info);
    row.raw.open_error=ERROR_ACCESS_DENIED; info.exit_observed=1;
    CHECK(strstr(ui_field_query_reason(&row,&info,0),"후속 조회")!=NULL);
    row.raw.open_error=0;
    /* A failed NAME query and subsequent observed exit cannot relabel the
     * successful CPU/IO counter's first sample as that API's query failure. */
    CHECK(ui_field_query_reason(&row,&info,0)==NULL);
    CHECK(ui_field_query_reason(&row,&info,ERROR_PARTIAL_COPY)!=NULL);
    row.raw.open_error=ERROR_INVALID_PARAMETER;
    CHECK(strstr(ui_field_query_reason(&row,&info,0),"확인되지")!=NULL);
    row.raw.open_error=0; row.path_error=ERROR_INVALID_DATA;
    CHECK(strstr(ui_path_reason(&row),"조회를 실행하지")!=NULL);
    row.raw.valid_fields=RESOURCE_VALID_IDENTITY;
    CHECK(strstr(ui_path_reason(&row),"생성시각이 달라")!=NULL);
}
static void test_access_display_and_causes(void)
{
    GuiSnapshot *view; MonitorRow *row; ResourceData before; PgSum sums[4];
    wchar_t text[16384],name[1024]; int visible; size_t i;
    init_app(); CHECK(create_hidden_window()!=NULL); view=test_view(1,0);
    view->display.items=calloc(view->metrics->count,sizeof(*view->display.items));
    CHECK(view->display.items!=NULL); if(!view->display.items) { gui_snapshot_free(view); clear_app(); return; }
    view->display.count=view->metrics->count;
    for(i=0;i<view->display.count;++i) {
        ResourceDisplayInfo *info=&view->display.items[i]; MonitorRow *r=&view->metrics->rows[i];
        info->pid=r->raw.pid; info->measured_at_utc_100ns=r->raw.measured_at_utc_100ns;
        info->creation_time_utc_100ns=(r->raw.valid_fields&RESOURCE_VALID_IDENTITY)?r->raw.creation_time_utc_100ns:0;
        info->name_source=RESOURCE_NAME_HANDLE;
    }
    row=&view->metrics->rows[3]; row->raw.valid_fields=0; row->raw.creation_time_utc_100ns=0;
    row->raw.open_error=ERROR_ACCESS_DENIED; row->raw.cpu_status=row->raw.io_read_status=row->raw.io_write_status=RESOURCE_DELTA_IDENTITY_UNAVAILABLE;
    row->raw.cpu_percent=row->raw.io_read_bytes_per_second=row->raw.io_write_bytes_per_second=NAN;
    free(row->path); row->path=NULL; row->program_id=0; row->path_error=ERROR_INVALID_DATA;
    view->display.items[3].name_source=RESOURCE_NAME_LIST;
    view->display.items[3].snapshot_name_utf8=_strdup("same-reference.exe");
    /* A separate known instance with exactly the same list name stays separate. */
    view->metrics->rows[2].raw.valid_fields&=~RESOURCE_VALID_NAME;
    view->display.items[2].name_source=RESOURCE_NAME_LIST;
    view->display.items[2].snapshot_name_utf8=_strdup("same-reference.exe");
    view->metrics->rows[0].raw.valid_fields&=~RESOURCE_VALID_NAME;
    view->display.items[0].name_source=RESOURCE_NAME_LIST;
    view->display.items[0].snapshot_name_utf8=_strdup("root-reference.exe");
    before=row->raw; app.latest=view; consume_snapshot();
    ListView_GetItemText(app.list,0,0,name,1024);
    CHECK(wcsstr(name,L"root-reference.exe")!=NULL); CHECK(wcsstr(name,L"목록 이름")!=NULL);
    select_item(0); detail_text(text,sizeof(text)/sizeof(text[0]));
    CHECK(wcsstr(text,L"root-reference.exe")!=NULL); CHECK(wcsstr(text,L"목록 이름")!=NULL);
    CHECK(app.groups->owner[2]!=app.groups->owner[3]);
    CHECK(app.groups->groups[app.groups->owner[3]].count==1);
    memcpy(sums,app.groups->groups[app.groups->owner[3]].metric,sizeof(sums));
    visible=index_for_pid(203); CHECK(visible>=0);
    ListView_GetItemText(app.list,visible,0,name,1024);
    CHECK(wcsstr(name,L"same-reference.exe")!=NULL); CHECK(wcsstr(name,L"목록 이름")!=NULL); CHECK(wcsstr(name,L"PID 203")!=NULL);
    select_item(visible); detail_text(text,sizeof(text)/sizeof(text[0]));
    CHECK(wcsstr(text,L"CPU NA")!=NULL); CHECK(wcsstr(text,L"RAM NA")!=NULL);
    CHECK(wcsstr(text,L"참고 이름")!=NULL); CHECK(wcsstr(text,L"후속 조회")!=NULL);
    CHECK(wcsstr(text,L"조회를 실행하지")!=NULL); CHECK(!IsWindowEnabled(GetDlgItem(app.window,ID_IGNORE)));
    CHECK(!memcmp(&before,&row->raw,sizeof(before)));
    search_for(L"same-reference.exe"); CHECK(ListView_GetItemCount(app.list)==2);
    CHECK(!memcmp(sums,app.groups->groups[app.groups->owner[3]].metric,sizeof(sums)));
    search_for(L"");
    /* A replacement snapshot with no sidecar cannot inherit the old name. */
    app.latest=test_view(1,0); app.latest->metrics->rows[3].raw.valid_fields&=~RESOURCE_VALID_NAME;
    consume_snapshot(); visible=index_for_pid(203); CHECK(visible>=0);
    ListView_GetItemText(app.list,visible,0,name,1024);
    CHECK(wcsstr(name,L"same-reference.exe")==NULL); CHECK(wcsstr(name,L"이름 확인 불가")!=NULL);
    CHECK(wcsstr(name,L"PID 203")!=NULL);
    clear_app();
}
