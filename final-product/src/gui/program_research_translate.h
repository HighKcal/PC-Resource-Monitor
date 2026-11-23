/* 확인된 공식 근거의 공개 원문 한 문장을 별도 동의 뒤 참고 번역하는 구현이다.
 * translation_preview가 보낼 문장을 제한하고 translate가 그 한 문장만 전송한다.
 * 번역은 신원·역할·시작 설정 판단을 새로 만들지 않으며 원문과 함께 표시할 참고 자료다. */
#ifndef PROGRAM_RESEARCH_TRANSLATE_H
#define PROGRAM_RESEARCH_TRANSLATE_H
static int pr_translation_forbidden(const char *text)
{
    static const char *words[]={"powershell","cmd.exe","command prompt","sudo ","ssh ","curl ","wget ","run the following","execute ","terminal command","password","username","user name","sign in","log in","login","api key","authorization","access token","account","credential","rm -","계정","암호","비밀번호","로그인","명령","인증 토큰"};
    size_t i;
    for(i=0;i<sizeof(words)/sizeof(words[0]);++i) if(pr_find_ci(text,words[i])) return 1;
    return 0;
}
/* 반환된 평문은 다시 전송하지 않는다. 기술 용어 자체와 실행/인증 지시는 구별한다. */
static int pr_translation_output_forbidden(const char *text)
{
    static const char *instructions[]={"powershell","cmd.exe","sudo ","ssh ","curl ","wget ","run the following","execute the ","password=","username=","authorization:","bearer ","api_key=","다음 명령","명령을 실행","명령 실행","실행하세요","입력하세요","비밀번호:","계정:","암호를 입력","비밀번호를 입력","계정에 로그인"};
    size_t i;
    for(i=0;i<sizeof(instructions)/sizeof(instructions[0]);++i) if(pr_find_ci(text,instructions[i])) return 1;
    return 0;
}
/* 유효한 공식 근거의 한 문장인지, 길이/문자/민감정보·명령 표현 제한을 만족하는지
 * 확인해 전송 미리보기로 복사한다. 네트워크 요청은 없으며 0이면 out을 전송하지 않는다. */
static int program_research_translation_preview(const ProgramResearchResult *source,unsigned index,char *out,size_t cap)
{
    const unsigned char *p; unsigned letters=0,digits=0;
    wchar_t wide[512]; size_t i;
    if(!out || !cap) return 0; out[0]=0;
    if(!source || index>=source->evidence_count || index>=3 ||
        !program_research_validate_opening_url(source,source->url) ||
        !source->evidence[index][0] || strlen(source->evidence[index])>500 ||
        pr_translation_forbidden(source->evidence[index]) ||
        !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,source->evidence[index],-1,wide,512)) return 0;
    for(i=0;wide[i];++i) if(wide[i]>=0xac00 && wide[i]<=0xd7a3) return 0;
    p=(const unsigned char *)source->evidence[index];
    while(*p) {
        if(*p<32 || *p==127 || strchr("\\/@<>`|;$",*p)) return 0;
        if(isalpha(*p) && *p<128) ++letters;
        if(isdigit(*p)) { if(++digits>=7) return 0; } else digits=0;
        if((*p=='.' || *p=='!' || *p=='?') && p[1] && isspace(p[1])) {
            const unsigned char *remaining=p+1;
            while(*remaining && isspace(*remaining)) ++remaining;
            if(*remaining) return 0;
        }
        if(isdigit(*p) && (p==(const unsigned char *)source->evidence[index] || !isdigit(p[-1]))) {
            const unsigned char *last=p; char possible[32]; size_t n; struct in_addr address;
            while(isdigit(*last) || *last=='.') ++last;
            n=(size_t)(last-p);
            if(n>=7 && n<sizeof(possible)) { memcpy(possible,p,n); possible[n]=0; if(InetPtonA(AF_INET,possible,&address)==1) return 0; }
        }
        ++p;
    }
    if(letters<20 || pr_find_ci(source->evidence[index],"http:") || pr_find_ci(source->evidence[index],"https:") ||
        pr_find_ci(source->evidence[index],"www.") || strstr(source->evidence[index],":\\")) return 0;
    return pr_copy(out,cap,source->evidence[index]);
}
static int pr_translation_decode(const char *body,size_t length,const char *original,char *out,size_t cap,DWORD *error)
{
    PrJson json={0}; char translated[2048],details[1024]; wchar_t wide[2048];
    int data,code,quota,valid=0,has_korean=0; size_t i;
    if(cap) out[0]=0;
    if(!pr_json_parse(&json,body,length)) { *error=ERROR_INVALID_DATA; return 0; }
    data=pr_json_field(&json,0,"responseData"); code=pr_json_field(&json,0,"responseStatus"); quota=pr_json_field(&json,0,"quotaFinished");
    if((!pr_json_literal(&json,code,"200") && !pr_json_equal(&json,code,"200")) ||
        (quota<0 || !pr_json_literal(&json,quota,"false"))) { *error=ERROR_RETRY; goto done; }
    if(pr_json_string(&json,pr_json_field(&json,0,"responseDetails"),details,sizeof(details)) && details[0]) { *error=ERROR_RETRY; goto done; }
    if(!pr_json_string(&json,pr_json_field(&json,data,"translatedText"),translated,sizeof(translated)) ||
        !translated[0] || !strcmp(translated,original) || pr_translation_output_forbidden(translated) ||
        strchr(translated,'<') || strchr(translated,'>') || strstr(translated,"https:") || strstr(translated,"http:") ||
        !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,translated,-1,wide,2048)) { *error=ERROR_INVALID_DATA; goto done; }
    for(i=0;wide[i];++i) if(wide[i]>=0xac00 && wide[i]<=0xd7a3) has_korean=1;
    if(has_korean && pr_copy(out,cap,translated)) valid=1; else *error=ERROR_INVALID_DATA;
done:
    free(json.tokens); return valid;
}
/* worker가 별도 동의를 받은 뒤 호출한다. 원문을 재확인하고 GET 한 번(리다이렉트 없음)
 * →응답 형식·한국어 확인→원문 유효성 재확인 순서로 진행한다. 실패/한도/취소를 정상 번역으로
 * 바꾸지 않는다. 원문·결과는 값으로 남고 네트워크 세션·응답·임시 transfer는 여기서 해제한다. */
static void program_research_translate(const ProgramResearchResult *source,unsigned index,int user_approved_translation,ProgramResearchTranslation *out)
{
    ProgramResearchResult *transfer;
    PrNet net; PrResponse response={0};
    WSADATA winsock; char encoded[1536],url[2048],translated[2048];
    DWORD protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    int startup;
    if(!out) return; memset(out,0,sizeof(*out)); out->status=PROGRAM_RESEARCH_NOT_REQUESTED;
    pr_copy(out->notice,sizeof(out->notice),"참고 번역 · 별도 동의한 공개 원문 한 문장만 MyMemory 전송 대상입니다. 약관상 장기 보관되거나 번역 파트너가 처리할 수 있습니다. 번역으로 신원·안전·원인·필요성을 판정하지 않습니다.");
    if(!source || !user_approved_translation) {
        pr_copy(out->notice,sizeof(out->notice),"참고 번역 미실행 · 별도 동의 없이는 공개 원문도 MyMemory에 전송하지 않습니다."); return;
    }
    out->source_request_id=source->request.request_id; out->evidence_index=index; out->status=PROGRAM_RESEARCH_UNKNOWN;
    if(!program_research_translation_preview(source,index,out->original,sizeof(out->original)) || !pr_escape_query(out->original,encoded,sizeof(encoded))) {
        pr_copy(out->notice,sizeof(out->notice),"참고 번역 미실행 · 현재 공식 근거·한 문장·길이·개인/명령 정보 제외 조건을 충족하지 못했습니다."); return;
    }
    transfer=(ProgramResearchResult *)calloc(1,sizeof(*transfer)); if(!transfer) { out->error=ERROR_NOT_ENOUGH_MEMORY; return; }
    transfer->request.cancel_event=source->request.cancel_event; transfer->request.deadline_tick=GetTickCount64()+15000;
    net.result=transfer; net.session=NULL;
    startup=WSAStartup(MAKEWORD(2,2),&winsock); if(startup) { transfer->error=(DWORD)startup; goto failed; }
    net.session=WinHttpOpen(L"PC-Resource-Monitor/0.1 (anonymous opt-in public sentence translation)",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!net.session) { transfer->error=GetLastError(); WSACleanup(); goto failed; }
    if(!WinHttpSetTimeouts(net.session,3000,3000,3000,3000) || !WinHttpSetOption(net.session,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols))) { transfer->error=GetLastError(); goto network_done; }
    snprintf(url,sizeof(url),"https://api.mymemory.translated.net/get?q=%s&langpair=en%%7Cko",encoded);
    if(!pr_fetch(&net,url,2,&response) || !pr_translation_decode(response.body,response.length,out->original,translated,sizeof(translated),&transfer->error) ||
        !program_research_translation_preview(source,index,encoded,sizeof(encoded))) goto network_done;
    pr_copy(out->translated_ko,sizeof(out->translated_ko),translated); out->status=PROGRAM_RESEARCH_COMPLETE;
network_done:
    pr_response_free(&response);
    WinHttpCloseHandle(net.session); WSACleanup();
failed:
    out->error=transfer->error;
    out->request_count=transfer->request_count;
    if(out->status!=PROGRAM_RESEARCH_COMPLETE)
        pr_copy(out->notice,sizeof(out->notice),transfer->request_count?"참고 번역 결과 미확인 · 요청을 시도했으나 유효한 한국어 결과를 확인하지 못했습니다. 한도·실패를 정상 번역으로 표시하지 않습니다.":"참고 번역 미실행 · 외부 요청을 시작하지 못했습니다.");
    if(out->status!=PROGRAM_RESEARCH_COMPLETE && out->error) out->status=PROGRAM_RESEARCH_FAILED;
    if(pr_cancelled(&transfer->request)) { out->status=PROGRAM_RESEARCH_CANCELLED; out->translated_ko[0]=0; }
    free(transfer);
}
#endif
