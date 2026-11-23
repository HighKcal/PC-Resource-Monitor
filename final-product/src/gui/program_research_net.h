/* 공개 자료 provider의 HTTPS 전송 구현이다. pr_fetch가 URL/DNS 검증→요청→제한된
 * 본문 수신을 맡고 전송 횟수/오류는 빌린 PrNet.result에 누적한다. 일반 조회와 번역이
 * 공유하며, 번역은 리다이렉트를 따르지 않는다. UI는 이 파일을 직접 호출하지 않는다. */
#ifndef PROGRAM_RESEARCH_NET_H
#define PROGRAM_RESEARCH_NET_H
typedef struct PrNet { HINTERNET session; ProgramResearchResult *result; } PrNet;
/* body는 수신 성공 시 이 응답이 소유하는 UTF-8 버퍼다. length는 NUL을 제외한 바이트 수,
 * 할당 용량은 BODY_LIMIT+1이다. 호출자는 사용 뒤 pr_response_free로 해제한다. */
typedef struct PrResponse { char *body; size_t length; char url[2048],content_type[256]; } PrResponse;
static int pr_same_web_host(const PrUrl *a,const PrUrl *b)
{
    const wchar_t *aa=a->host,*bb=b->host;
    if(!wcsncmp(aa,L"www.",4)) aa+=4;
    if(!wcsncmp(bb,L"www.",4)) bb+=4;
    return !wcscmp(aa,bb);
}
static int pr_public_address(const struct sockaddr *address)
{
    if(address->sa_family==AF_INET) {
        const unsigned char *a=(const unsigned char *)&((const struct sockaddr_in *)address)->sin_addr;
        return a[0]!=0 && a[0]!=10 && a[0]!=127 && a[0]<224 &&
            !(a[0]==100 && a[1]>=64 && a[1]<=127) && !(a[0]==169 && a[1]==254) &&
            !(a[0]==172 && a[1]>=16 && a[1]<=31) && !(a[0]==192 && (a[1]==0 || a[1]==168 || (a[1]==88 && a[2]==99))) &&
            !(a[0]==198 && (a[1]==18 || a[1]==19 || (a[1]==51 && a[2]==100))) &&
            !(a[0]==203 && a[1]==0 && a[2]==113);
    }
    if(address->sa_family==AF_INET6) {
        const unsigned char *a=((const struct sockaddr_in6 *)address)->sin6_addr.u.Byte;
        return (a[0]&0xe0)==0x20 && !(a[0]==0x20 && a[1]==2) &&
            !(a[0]==0x20 && a[1]==1 && ((a[2]==0 && a[3]==0) ||
            (a[2]==0x0d && a[3]==0xb8) || (a[2]==0 && (a[3]&0xf0)==0x10) || (a[2]==0 && (a[3]&0xf0)==0x20)));
    }
    return 0;
}
/* DNS 결과를 모두 검사하고 그중 한 IP를 pin한다. 원래 host는 TLS/HTTP에 유지한다.
 * RESOLUTION_HOSTNAME을 지원하지 않는 OS는 network를 시작하지 않고 실패한다. */
static int pr_resolve(const ProgramResearchRequest *request,const wchar_t *host,wchar_t *numeric,size_t cap,DWORD *error)
{
    ADDRINFOEXW hints,*answers=NULL,*item,*chosen=NULL;
    OVERLAPPED operation; HANDLE cancellation=NULL;
    typedef INT (WSAAPI *PrDnsCancel)(LPHANDLE);
    typedef INT (WSAAPI *PrDnsResult)(LPOVERLAPPED);
    PrDnsCancel cancel=(PrDnsCancel)(void *)GetProcAddress(GetModuleHandleW(L"ws2_32.dll"),"GetAddrInfoExCancel");
    PrDnsResult completion=(PrDnsResult)(void *)GetProcAddress(GetModuleHandleW(L"ws2_32.dll"),"GetAddrInfoExOverlappedResult");
    uint64_t started=GetTickCount64();
    unsigned count=0;
    int status;
    if(!cancel || !completion) { *error=ERROR_NOT_SUPPORTED; return 0; }
    memset(&operation,0,sizeof(operation)); operation.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!operation.hEvent) { *error=GetLastError(); return 0; }
    memset(&hints,0,sizeof(hints)); hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM; hints.ai_protocol=IPPROTO_TCP;
    status=GetAddrInfoExW(host,L"443",NS_DNS,NULL,&hints,&answers,NULL,&operation,NULL,&cancellation);
    if(status==WSA_IO_PENDING) {
        while(WaitForSingleObject(operation.hEvent,50)==WAIT_TIMEOUT) {
            if(pr_cancelled(request) || GetTickCount64()-started>=3000) {
                (void)cancel(&cancellation);
                /* 취소 완료까지 출력 포인터/OVERLAPPED의 수명을 보존한다. */
                WaitForSingleObject(operation.hEvent,INFINITE); break;
            }
        }
        status=completion(&operation);
    }
    CloseHandle(operation.hEvent);
    if(status) { if(answers) FreeAddrInfoExW(answers); *error=(DWORD)status; return 0; }
    for(item=answers;item;item=item->ai_next) {
        if(++count>16 || !item->ai_addr || !pr_public_address(item->ai_addr)) {
            FreeAddrInfoExW(answers); *error=ERROR_ACCESS_DENIED; return 0;
        }
        if(!chosen || item->ai_family==AF_INET) chosen=item;
    }
    if(!chosen || cap>INT_MAX || GetNameInfoW(chosen->ai_addr,(int)chosen->ai_addrlen,
        numeric,(DWORD)cap,NULL,0,NI_NUMERICHOST)) {
        FreeAddrInfoExW(answers); *error=ERROR_HOST_UNREACHABLE; return 0;
    }
    FreeAddrInfoExW(answers); return 1;
}
static void pr_response_free(PrResponse *response)
{
    free(response->body); memset(response,0,sizeof(*response));
}
static int pr_header(HINTERNET request,DWORD query,char *out,size_t cap)
{
    wchar_t value[2048]; DWORD bytes=sizeof(value);
    if(!WinHttpQueryHeaders(request,query,WINHTTP_HEADER_NAME_BY_INDEX,value,&bytes,WINHTTP_NO_HEADER_INDEX)) return 0;
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,out,(int)cap,NULL,NULL)>0;
}
/* 비어 있는 response에 한 응답을 채운다. 성공이면 본문 소유권을 호출자에게 넘기고
 * 실패면 부분 본문·요청·연결을 해제한다. net의 세션은 닫지 않으며 lookup/translate가 닫는다.
 * internal_query=0은 문서, 1은 내부 API query, 2는 별도 동의한 번역 GET 한 번이다. */
static int pr_fetch(PrNet *net,const char *url,int internal_query,PrResponse *response)
{
    char current[2048]; unsigned redirects=0;
    memset(response,0,sizeof(*response));
    if(!pr_copy(current,sizeof(current),url)) return 0;
    while(!pr_cancelled(&net->result->request) && net->result->request_count<PROGRAM_RESEARCH_REQUEST_LIMIT) {
        PrUrl parsed,previous;
        wchar_t address[128];
        HINTERNET connection=NULL,request=NULL;
        DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION|WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_KEEP_ALIVE;
        DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER,autologon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        DWORD status=0,size=sizeof(status),received=0;
        BOOL reject=TRUE;
        int ok=0;
        if(!pr_parse_url(current,internal_query,&parsed)) { net->result->error=ERROR_INVALID_NAME; return 0; }
        previous=parsed;
        if(!pr_resolve(&net->result->request,parsed.host,address,sizeof(address)/sizeof(*address),&net->result->error)) return 0;
        if(pr_cancelled(&net->result->request)) return 0;
        connection=WinHttpConnect(net->session,parsed.host,INTERNET_DEFAULT_HTTPS_PORT,0);
        if(connection) request=WinHttpOpenRequest(connection,L"GET",parsed.path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        if(!request) goto finished;
        if(!WinHttpSetOption(request,WINHTTP_OPTION_RESOLUTION_HOSTNAME,address,(DWORD)((wcslen(address)+1)*sizeof(wchar_t))) ||
            !WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)) ||
            !WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)) ||
            !WinHttpSetOption(request,WINHTTP_OPTION_AUTOLOGON_POLICY,&autologon,sizeof(autologon)) ||
            !WinHttpSetOption(request,WINHTTP_OPTION_REJECT_USERPWD_IN_URL,&reject,sizeof(reject))) goto finished;
        if(!WinHttpSetOption(request,WINHTTP_OPTION_CLIENT_CERT_CONTEXT,WINHTTP_NO_CLIENT_CERT_CONTEXT,0)) goto finished;
        ++net->result->request_count;
        if(!WinHttpSendRequest(request,L"Accept: text/html, application/json;q=0.9\r\nAccept-Language: ko-KR,ko;q=0.9,en;q=0.7\r\n",(DWORD)-1L,
            WINHTTP_NO_REQUEST_DATA,0,0,0) || !WinHttpReceiveResponse(request,NULL)) goto finished;
        if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)) goto finished;
        net->result->http_status=status;
        if(status>=300 && status<400) {
            char location[2048],next[2048];
            if(internal_query==2) goto finished; /* 명시적 번역 동의 한 번은 GET 한 번뿐이다. */
            if(redirects++>=2 || !pr_header(request,WINHTTP_QUERY_LOCATION,location,sizeof(location))) goto finished;
            if(location[0]=='/' && location[1]!='/') {
                if(strlen(parsed.origin)+strlen(location)>=sizeof(next)) goto finished;
                snprintf(next,sizeof(next),"%s%s",parsed.origin,location);
            } else if(location[0]=='/' && location[1]=='/') {
                if(strlen(location)+6>=sizeof(next)) goto finished;
                snprintf(next,sizeof(next),"https:%s",location);
            } else if(!pr_copy(next,sizeof(next),location)) goto finished;
            /* 모든 redirect는 다시 URL/DNS 검증한다. HTTP·인증·query로 우회하지 않는다. */
            if(!pr_parse_url(next,internal_query,&parsed) ||
                (internal_query?strcmp(previous.origin,parsed.origin)!=0:!pr_same_web_host(&previous,&parsed)) ||
                !pr_copy(current,sizeof(current),parsed.url)) goto finished;
            WinHttpCloseHandle(request); WinHttpCloseHandle(connection); continue;
        }
        if(status!=200) { net->result->error=ERROR_WINHTTP_INVALID_SERVER_RESPONSE; goto finished; }
        if(!pr_header(request,WINHTTP_QUERY_CONTENT_TYPE,response->content_type,sizeof(response->content_type)) ||
            (!strstr(response->content_type,"text/html") && !strstr(response->content_type,"application/json") && !strstr(response->content_type,"application/ld+json"))) goto finished;
        response->body=(char *)malloc(PROGRAM_RESEARCH_BODY_LIMIT+1);
        if(!response->body) { net->result->error=ERROR_NOT_ENOUGH_MEMORY; goto finished; }
        while(!pr_cancelled(&net->result->request)) {
            DWORD available=(DWORD)(PROGRAM_RESEARCH_BODY_LIMIT-response->length);
            if(!available) { net->result->error=ERROR_BUFFER_OVERFLOW; goto finished; }
            if(available>16384) available=16384;
            if(!WinHttpReadData(request,response->body+response->length,available,&received)) goto finished;
            if(!received) { ok=1; break; }
            response->length+=received;
        }
        if(ok) {
            response->body[response->length]=0;
            if(memchr(response->body,0,response->length)) ok=0;
            else if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,response->body,(int)response->length,NULL,0)) ok=0;
            else pr_copy(response->url,sizeof(response->url),current);
        }
finished:
        if(!ok && !net->result->error) net->result->error=GetLastError();
        if(request) WinHttpCloseHandle(request);
        if(connection) WinHttpCloseHandle(connection);
        if(!ok) pr_response_free(response);
        return ok;
    }
    net->result->error=pr_cancelled(&net->result->request)?ERROR_CANCELLED:ERROR_TOO_MANY_CMDS;
    return 0;
}
#endif
