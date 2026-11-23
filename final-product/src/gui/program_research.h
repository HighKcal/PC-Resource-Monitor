/* 사용자 승인 뒤 공개 제품 정보만 조사하는 별도 worker용 provider의 타입과 URL 규칙이다.
 * PID·경로·계정·자원 측정값을 입력 구조에 넣지 않는다. 웹 문장은 실행하지 않는 데이터다.
 * plan/lookup은 program_research_lookup.h, 실제 GET은 net, 응답 해석은 json/html,
 * 후보 발견은 wikidata, 별도 동의한 번역은 translate 구현 조각에서 읽는다. */
#ifndef PROGRAM_RESEARCH_H
#define PROGRAM_RESEARCH_H
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <limits.h>
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"ws2_32.lib")
#pragma warning(push)
#pragma warning(disable:4505)

#define PROGRAM_RESEARCH_BODY_LIMIT (1024u*1024u)
#define PROGRAM_RESEARCH_REQUEST_LIMIT 12u
#define PROGRAM_RESEARCH_LIFETIME_MS 60000u
typedef enum ProgramResearchHomepageSource {
    PROGRAM_RESEARCH_HOME_NONE=0, PROGRAM_RESEARCH_HOME_SIGNED_OPUS_PRODUCT,
    PROGRAM_RESEARCH_HOME_SIGNED_OPUS_PUBLISHER, PROGRAM_RESEARCH_HOME_SIGNED_VERSION
} ProgramResearchHomepageSource;
typedef enum ProgramResearchStatus {
    PROGRAM_RESEARCH_NOT_REQUESTED=0,PROGRAM_RESEARCH_COMPLETE,PROGRAM_RESEARCH_PARTIAL,
    PROGRAM_RESEARCH_UNKNOWN,PROGRAM_RESEARCH_FAILED,PROGRAM_RESEARCH_CANCELLED
} ProgramResearchStatus;
/* 문자열은 값 복사, cancel_event는 호출자가 작업이 끝날 때까지 보존할 빌린 핸들이다.
 * deadline_tick은 GetTickCount64 밀리초 기한이며 기록용 UTC와 섞어 계산하지 않는다. */
typedef struct ProgramResearchRequest {
    uint64_t request_id;
    unsigned session,intent; /* 목적은 로컬 설명에만 사용하며 검색 query에 넣지 않는다. */
    int user_approved_public,local_identity_verified;
    HANDLE cancel_event;
    uint64_t deadline_tick;
    char product[256],description[512],company[256],publisher[256],original[256];
    char homepage_origin[256];
    ProgramResearchHomepageSource homepage_source;
} ProgramResearchRequest;
typedef struct ProgramResearchResult {
    ProgramResearchRequest request;
    ProgramResearchStatus status;
    uint64_t fetched_utc,checked_tick,expires_tick;
    DWORD error,http_status;
    int official_confirmed,product_matched;
    char title[512],url[1024],origin[256],discovery_url[1024],authority_text[1024];
    char matched_product[256],ownership_url[1024],ownership_evidence[1024];
    char summary_ko[4096],evidence[3][2048],page_text[32768],failure_text[1024];
    unsigned evidence_count,request_count,evidence_meta_mask; /* bit i: HTML 문서 소개(meta)에서 가져온 원문 */
} ProgramResearchResult;
typedef struct ProgramResearchTranslation {
    uint64_t source_request_id;
    unsigned evidence_index;
    ProgramResearchStatus status;
    DWORD error;
    unsigned request_count;
    char original[512],translated_ko[2048],notice[1024];
} ProgramResearchTranslation;
typedef struct ProgramResearchPlan {
    char homepage_origin[256],wikidata_search_url[1536],disclosure_ko[2048];
} ProgramResearchPlan;
typedef struct PrUrl {
    wchar_t host[256],path[2048];
    char origin[256],url[2048];
} PrUrl;
static int pr_copy(char *out,size_t cap,const char *text)
{
    size_t n=strlen(text);
    if(n>=cap) { if(cap) out[0]=0; return 0; }
    memcpy(out,text,n+1); return 1;
}
static int pr_cancelled(const ProgramResearchRequest *request)
{
    return (request->cancel_event && WaitForSingleObject(request->cancel_event,0)!=WAIT_TIMEOUT) ||
        (request->deadline_tick && GetTickCount64()>=request->deadline_tick);
}
/* URL은 ASCII HTTPS FQDN, 표준443, 무자격증명만 허용한다. 경로를 보존할 때도
 * query/fragment와 역슬래시·제어문자를 허용하지 않는다. API query는 내부 함수만 만든다. */
static int pr_parse_url(const char *url,int internal_query,PrUrl *out)
{
    const char *host,*end,*path,*scan;
    size_t host_len,path_len,i,label=0;
    int dots=0,alpha=0;
    memset(out,0,sizeof(*out));
    if(!url || strncmp(url,"https://",8) || strlen(url)>=sizeof(out->url)) return 0;
    for(scan=url;*scan;++scan) if((unsigned char)*scan<=32 || (unsigned char)*scan>=127 ||
        *scan=='\\' || *scan=='@' || *scan=='#' || (!internal_query && *scan=='?')) return 0;
    host=url+8; end=host;
    while(*end && *end!='/' && *end!='?') ++end;
    host_len=(size_t)(end-host);
    if(host_len>4 && !memcmp(end-4,":443",4)) host_len-=4;
    if(!host_len || host_len>=sizeof(out->host)/sizeof(out->host[0]) || host[0]=='-' || host[host_len-1]=='-') return 0;
    for(i=0;i<host_len;++i) {
        unsigned char c=(unsigned char)host[i];
        if(c=='.') {
            if(!label || label>63 || host[i-1]=='-') return 0;
            ++dots; label=0;
        } else {
            if(!isalnum(c) && c!='-') return 0;
            if(!label && c=='-') return 0;
            ++label; if(isalpha(c)) alpha=1;
        }
        out->host[i]=(wchar_t)tolower(c);
    }
    if(!dots || !alpha || !label || label>63 || !isalpha((unsigned char)host[host_len-1])) return 0;
    /* 로컬 이름 공간은 공개 FQDN으로 조사하지 않는다. */
    if(wcsstr(out->host,L".localhost") || wcsstr(out->host,L".local") ||
        wcsstr(out->host,L".internal") || wcsstr(out->host,L".home") ||
        wcsstr(out->host,L".lan") || wcsstr(out->host,L".invalid") || wcsstr(out->host,L".test")) return 0;
    path=*end=='/'?end:(*end=='?'?end:"/"); path_len=strlen(path);
    if(path_len>=sizeof(out->path)/sizeof(out->path[0])) return 0;
    for(i=0;i<path_len;++i) out->path[i]=(wchar_t)(unsigned char)path[i];
    if(!WideCharToMultiByte(CP_UTF8,0,out->host,-1,out->origin+8,(int)sizeof(out->origin)-8,NULL,NULL)) return 0;
    memcpy(out->origin,"https://",8);
    if(strlen(out->origin)+path_len>=sizeof(out->url)) return 0;
    snprintf(out->url,sizeof(out->url),"%s%s",out->origin,path);
    return 1;
}
static int program_research_origin(const char *url,char *origin,size_t cap)
{
    PrUrl parsed;
    if(!origin || !cap) return 0;
    origin[0]=0;
    return pr_parse_url(url,0,&parsed) && pr_copy(origin,cap,parsed.origin);
}
/* 결과 보관 중 링크를 쓸 때 호출한다. 공식/제품 연결·인용·성공 상태·60초 유효기간과
 * 결과의 정확한 URL/origin을 함께 확인한다. TTL 안이라는 이유만으로 임의 URL을 허용하지 않는다. */
static int program_research_validate_opening_url(const ProgramResearchResult *result,const char *url)
{
    PrUrl parsed;
    return result && result->official_confirmed && result->product_matched && result->evidence_count &&
        (result->status==PROGRAM_RESEARCH_COMPLETE || result->status==PROGRAM_RESEARCH_PARTIAL) &&
        GetTickCount64()<result->expires_tick && !strcmp(result->url,url) && pr_parse_url(url,0,&parsed) &&
        !strcmp(parsed.origin,result->origin);
}

#include "program_research_net.h"
#include "program_research_json.h"
#include "program_research_html.h"
#include "program_research_lookup.h"
#include "program_research_translate.h"
#pragma warning(pop)
#endif
