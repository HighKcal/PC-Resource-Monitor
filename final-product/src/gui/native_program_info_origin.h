/* program_info_homepage가 쓰는 로컬 HTTPS origin 필터다. URL에서 경로를 빼고
 * 공개 호스트 단서만 남긴다. 자격증명/query 등은 거절하며 네트워크 요청은 하지 않는다.
 * 서명자가 제공한 주소도 제품 설명의 증명은 아니다. 실제 전송 전 DNS 검사는
 * program_research_net.h가 맡고, 페이지와 제품의 연결은 program_research_lookup.h가 판단한다. */
#ifndef MONITOR_NATIVE_PROGRAM_INFO_ORIGIN_H
#define MONITOR_NATIVE_PROGRAM_INFO_ORIGIN_H
static int program_info_https_origin(const char *url,char *origin,size_t cap)
{
    const char *end,*dot;
    size_t length,label=0;
    int alpha=0;
    if(!origin || cap<10) return 0;
    origin[0]=0;
    if(!url || _strnicmp(url,"https://",8)) return 0;
    for(const char *p=url;*p;++p) if((unsigned char)*p<=32 || (unsigned char)*p>=127 ||
        *p=='?' || *p=='#' || *p=='@' || *p=='\\') return 0;
    end=url+8;
    while(*end && *end!='/' && *end!='?' && *end!='#') ++end;
    length=(size_t)(end-(url+8));
    if(length>4 && !memcmp(url+8+length-4,":443",4)) length-=4;
    if(length<4 || length>253 || length+9>cap) return 0;
    for(size_t i=0;i<length;++i) {
        unsigned char c=(unsigned char)url[8+i];
        if(c>='A' && c<='Z') c=(unsigned char)(c-'A'+'a');
        if(c=='.') {
            if(!label || label>63 || origin[8+i-1]=='-') return 0;
            label=0;
        } else {
            if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-') || (!label && c=='-')) return 0;
            if(c>='a' && c<='z') alpha=1;
            ++label;
        }
        origin[8+i]=(char)c;
    }
    if(!label || label>63 || origin[8+length-1]=='-' || !alpha) { origin[0]=0; return 0; }
    memcpy(origin,"https://",8); origin[8+length]=0;
    dot=strrchr(origin+8,'.');
    if(!dot || strlen(dot+1)<2 || !_stricmp(dot,".local") || !_stricmp(dot,".internal") ||
        !_stricmp(dot,".localhost") || !_stricmp(dot,".lan") || !_stricmp(dot,".home") ||
        !_stricmp(dot,".invalid") || !_stricmp(dot,".test") || !_stricmp(dot,".example")) { origin[0]=0; return 0; }
    for(const char *p=dot+1;*p;++p) if(*p<'a' || *p>'z') { origin[0]=0; return 0; }
    return 1;
}
#endif
