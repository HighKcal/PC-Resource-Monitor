/* 외부 HTML을 실행 없이 제품 확인용 제목·평문·링크로 추리는 내부 구현이다.
 * pr_html의 성공은 읽을 텍스트가 있다는 뜻이며 공식성/제품 일치는 lookup이 따로 확인한다.
 * PrPage는 고정 배열의 값만 소유하므로 응답 body를 해제해도 추린 결과는 남는다. */
#ifndef PROGRAM_RESEARCH_HTML_H
#define PROGRAM_RESEARCH_HTML_H
typedef struct PrLink { char url[2048],text[256]; unsigned priority; } PrLink;
typedef struct PrPage {
    char title[512],description[4096],text[32768],legal[4096],ko_url[2048];
    PrLink links[12]; unsigned link_count;
    PrLink owner_links[2]; unsigned owner_link_count;
} PrPage;
static const char *pr_find_ci(const char *text,const char *find)
{
    size_t n=strlen(find);
    if(!n) return text;
    while(*text) { if(!_strnicmp(text,find,n)) return text; ++text; }
    return NULL;
}
/* 제품/조직 문자열 대조용 키를 만든다. company일 때만 법인 표기 토큰을 제외한다.
 * 이 정규화가 실제 프로세스 신원을 확인하는 것은 아니며 원문 표시는 별도 값으로 보존한다. */
static int pr_normalize(const char *input,int company,char *out,size_t cap)
{
    size_t i=0,used=0;
    static const char *suffixes[]={"corporation","corp","inc","incorporated","llc","ltd","limited","gmbh","company","co","ab","plc"};
    out[0]=0;
    while(input[i]) {
        if((unsigned char)input[i]<128 && isalnum((unsigned char)input[i])) {
            char word[256]; size_t j=0,k; int skip=0;
            while(input[i] && (unsigned char)input[i]<128 && isalnum((unsigned char)input[i])) {
                if(j+1>=sizeof(word)) return 0;
                word[j++]=(char)tolower((unsigned char)input[i++]);
            }
            word[j]=0;
            if(company) for(k=0;k<sizeof(suffixes)/sizeof(suffixes[0]);++k) if(!strcmp(word,suffixes[k])) skip=1;
            if(!skip) { if(used+j>=cap) return 0; memcpy(out+used,word,j); used+=j; }
        } else if((unsigned char)input[i]>=128) {
            unsigned char c=(unsigned char)input[i]; size_t count=c<0xe0?2:c<0xf0?3:4;
            if(!strncmp(input+i,"\xc2\xae",2) || !strncmp(input+i,"\xe2\x84\xa2",3)) { i+=count; continue; }
            if(strlen(input+i)<count || used+count>=cap) return 0;
            memcpy(out+used,input+i,count); used+=count; i+=count;
        } else ++i;
    }
    out[used]=0; return used>0;
}
static int pr_identity_equal(const char *a,const char *b,int company)
{
    char aa[512],bb[512];
    return pr_normalize(a,company,aa,sizeof(aa)) && pr_normalize(b,company,bb,sizeof(bb)) && !strcmp(aa,bb);
}
static int pr_mentions(const char *text,const char *name,int company)
{
    char normalized[32768],wanted[512];
    const char *inputs[2]={text,name}; char *outputs[2]={normalized,wanted};
    size_t capacities[2]={sizeof(normalized),sizeof(wanted)},which;
    for(which=0;which<2;++which) {
        size_t used=0,i=0; const char *input=inputs[which]; char *out=outputs[which];
        while(input[i]) {
            size_t begin=i,n; char token[512],key[512];
            while(input[i] && (isalnum((unsigned char)input[i]) || (unsigned char)input[i]>=128)) ++i;
            n=i-begin;
            if(n) {
                if(n>=sizeof(token)) return 0;
                memcpy(token,input+begin,n); token[n]=0;
                if(!pr_normalize(token,company,key,sizeof(key))) key[0]=0;
                if(key[0]) {
                    n=strlen(key); if(used+n+2>=capacities[which]) return 0;
                    if(used) out[used++]=' '; memcpy(out+used,key,n); used+=n;
                }
            } else ++i;
        }
        out[used]=0;
    }
    if(strlen(wanted)>=3) {
        const char *found=normalized; size_t length=strlen(wanted);
        while((found=strstr(found,wanted))!=NULL) {
            if((found==normalized || found[-1]==' ') && (!found[length] || found[length]==' ')) return 1;
            ++found;
        }
    }
    return 0;
}
static void pr_append(char *out,size_t cap,const char *text)
{
    size_t used=strlen(out),length=strlen(text);
    if(length>=cap-used) length=cap-used-1;
    while(length && ((unsigned char)text[length]&0xc0)==0x80) --length;
    memcpy(out+used,text,length); out[used+length]=0;
}
static void pr_plain(const char *source,size_t length,char *out,size_t cap)
{
    size_t i=0,used=0; int space=1;
    while(i<length && used+1<cap) {
        unsigned char c=(unsigned char)source[i++];
        if(c=='&') {
            size_t end=i; unsigned scalar=0; int known=0;
            while(end<length && end-i<16 && source[end]!=';' && source[end]!=' ' && source[end]!='<') ++end;
            if(end<length && source[end]==';') {
                size_t n=end-i;
                if(n==3 && !memcmp(source+i,"amp",3)) scalar='&',known=1;
                else if(n==2 && !memcmp(source+i,"lt",2)) scalar='<',known=1;
                else if(n==2 && !memcmp(source+i,"gt",2)) scalar='>',known=1;
                else if(n==4 && !memcmp(source+i,"quot",4)) scalar='"',known=1;
                else if(n==4 && !memcmp(source+i,"apos",4)) scalar='\'',known=1;
                else if(n==4 && !memcmp(source+i,"nbsp",4)) scalar=' ',known=1;
                else if(n==4 && !memcmp(source+i,"copy",4)) scalar=0xa9,known=1;
                else if(n>1 && source[i]=='#') {
                    size_t j=i+1; unsigned base=10; known=1;
                    if(source[j]=='x' || source[j]=='X') { base=16; ++j; }
                    for(;j<end;++j) { int digit=pr_hex(source[j]); if(digit<0 || (unsigned)digit>=base || scalar>0x10ffff/base) { known=0; break; } scalar=scalar*base+(unsigned)digit; }
                }
                if(known) {
                    i=end+1;
                    if(scalar<33 || scalar==160) c=' ';
                    else { if(!pr_utf8_scalar(scalar,out,cap,&used)) break; space=0; continue; }
                }
            }
        }
        if(c<33 || c==127) { if(!space) out[used++]=' '; space=1; }
        else { out[used++]=(char)c; space=0; }
    }
    while(used && out[used-1]==' ') --used;
    if(used) {
        size_t start=used-1,count,actual;
        while(start && ((unsigned char)out[start]&0xc0)==0x80) --start;
        count=(unsigned char)out[start]<0x80?1:(unsigned char)out[start]<0xe0?2:(unsigned char)out[start]<0xf0?3:4;
        actual=used-start;
        if(actual<count) used=start;
    }
    out[used]=0;
}
static int pr_attribute(const char *tag,size_t length,const char *name,char *out,size_t cap)
{
    size_t i=0,wanted=strlen(name);
    while(i<length) {
        size_t start,end,value_start,value_end; char quote=0;
        while(i<length && !(isalnum((unsigned char)tag[i]) || tag[i]=='_' || tag[i]=='-')) ++i;
        start=i; while(i<length && (isalnum((unsigned char)tag[i]) || tag[i]=='_' || tag[i]=='-' || tag[i]==':')) ++i;
        end=i; while(i<length && isspace((unsigned char)tag[i])) ++i;
        if(i>=length || tag[i]!='=') { if(i==start) ++i; continue; }
        ++i; while(i<length && isspace((unsigned char)tag[i])) ++i;
        if(i<length && (tag[i]=='"' || tag[i]=='\'')) quote=tag[i++];
        value_start=i;
        while(i<length && (quote?tag[i]!=quote:!isspace((unsigned char)tag[i]) && tag[i]!='>')) ++i;
        value_end=i; if(quote && i<length) ++i;
        if(end-start==wanted && !_strnicmp(tag+start,name,wanted)) {
            if(value_end-value_start>=cap) return 0;
            pr_plain(tag+value_start,value_end-value_start,out,cap); return out[0]!=0;
        }
    }
    if(cap) out[0]=0; return 0;
}
static int pr_link_url(const char *base,const char *href,char *out,size_t cap)
{
    PrUrl parsed,link;
    char combined[2048],clean[2048],*fragment;
    if(!pr_parse_url(base,0,&parsed)) return 0;
    if(!pr_copy(clean,sizeof(clean),href)) return 0;
    fragment=strchr(clean,'#'); if(fragment) *fragment=0; href=clean;
    if(!*href) return pr_copy(out,cap,parsed.url);
    if(!strncmp(href,"https://",8)) return pr_parse_url(href,0,&link) && pr_copy(out,cap,link.url);
    if(href[0]=='/' && href[1]!='/') {
        if(strlen(parsed.origin)+strlen(href)>=sizeof(combined)) return 0;
        snprintf(combined,sizeof(combined),"%s%s",parsed.origin,href);
    } else {
        const char *slash=strrchr(parsed.url,'/'); size_t prefix=slash?(size_t)(slash-parsed.url+1):0;
        if(!prefix || href[0]=='?' || strchr(href,':') || strstr(href,"..") || prefix+strlen(href)>=sizeof(combined)) return 0;
        memcpy(combined,parsed.url,prefix); strcpy_s(combined+prefix,sizeof(combined)-prefix,href);
    }
    return pr_parse_url(combined,0,&link) && pr_copy(out,cap,link.url);
}
static unsigned pr_owner_priority(const PrLink *link)
{
    static const char *primary[]={"about","team","governance","maintainer","who-we-are","소개","개발팀"};
    static const char *secondary[]={"legal","imprint","terms"}; size_t i;
    for(i=0;i<sizeof(primary)/sizeof(primary[0]);++i)
        if(pr_find_ci(link->text,primary[i]) || pr_find_ci(link->url,primary[i])) return 2;
    for(i=0;i<sizeof(secondary)/sizeof(secondary[0]);++i)
        if(pr_find_ci(link->text,secondary[i]) || pr_find_ci(link->url,secondary[i])) return 1;
    return 0;
}
static void pr_keep_owner_link(PrPage *page,const PrLink *link,const char *base)
{
    unsigned priority=pr_owner_priority(link),i,slot=0; PrUrl from,to;
    if(!priority || !pr_parse_url(base,0,&from) || !pr_parse_url(link->url,0,&to) || strcmp(from.origin,to.origin)) return;
    for(i=0;i<page->owner_link_count;++i) if(!strcmp(page->owner_links[i].url,link->url)) return;
    if(page->owner_link_count<2) slot=page->owner_link_count++;
    else {
        if(page->owner_links[1].priority<page->owner_links[0].priority) slot=1;
        if(page->owner_links[slot].priority>=priority) return;
    }
    page->owner_links[slot]=*link; page->owner_links[slot].priority=priority;
}
/* script/style 등의 내용은 건너뛰고 본문·소개(meta)·법적 표기를 나눈다. 같은 origin의
 * 한국어 대체 URL과 운영주체 확인 링크는 후속 조회 후보로만 보관한다. 고정 버퍼에 맞춰
 * 텍스트/링크 수를 제한하므로 전체 브라우저 DOM이나 페이지의 완전한 재현은 아니다. */
static int pr_html(const PrResponse *response,PrPage *page)
{
    const char *html=response->body,*cursor=html,*end=html+response->length;
    char skip[16]={0},anchor[2048]={0}; int footer=0,in_title=0,in_anchor=0; PrLink active;
    memset(page,0,sizeof(*page));
    while(cursor<end) {
        if(*cursor=='<') {
            const char *tag=cursor+1,*close=tag; char quote=0,name[32]={0}; size_t n=0;
            int ending=0;
            if(end-cursor>=4 && !memcmp(cursor,"<!--",4)) {
                const char *comment=strstr(cursor+4,"-->"); cursor=comment?comment+3:end; continue;
            }
            while(close<end) { if(quote) { if(*close==quote) quote=0; } else if(*close=='"' || *close=='\'') quote=*close; else if(*close=='>') break; ++close; }
            if(close==end) break;
            if(*tag=='/') { ending=1; ++tag; }
            while(tag+n<close && n+1<sizeof(name) && (isalnum((unsigned char)tag[n]) || tag[n]=='-')) { name[n]=(char)tolower((unsigned char)tag[n]); ++n; }
            if(skip[0]) { if(ending && !strcmp(name,skip)) skip[0]=0; cursor=close+1; continue; }
            if(!ending && (!strcmp(name,"script") || !strcmp(name,"style") || !strcmp(name,"noscript") || !strcmp(name,"svg") || !strcmp(name,"template"))) { pr_copy(skip,sizeof(skip),name); cursor=close+1; continue; }
            if(!strcmp(name,"title")) in_title=!ending;
            if(!strcmp(name,"footer")) footer=!ending;
            if(!ending && !strcmp(name,"meta")) {
                char property[128],content[4096];
                if(!pr_attribute(tag,(size_t)(close-tag),"property",property,sizeof(property))) pr_attribute(tag,(size_t)(close-tag),"name",property,sizeof(property));
                if(pr_attribute(tag,(size_t)(close-tag),"content",content,sizeof(content))) {
                    if(!_stricmp(property,"description") || !_stricmp(property,"og:description")) { if(!page->description[0]) pr_copy(page->description,sizeof(page->description),content); }
                    if(!_stricmp(property,"og:title") && !page->title[0]) pr_copy(page->title,sizeof(page->title),content);
                }
            }
            if(!ending && !strcmp(name,"link")) {
                char relation[128],language[32],href[2048],url[2048]; PrUrl from,to;
                if(pr_attribute(tag,(size_t)(close-tag),"rel",relation,sizeof(relation)) &&
                    pr_find_ci(relation,"alternate") && pr_attribute(tag,(size_t)(close-tag),"hreflang",language,sizeof(language)) &&
                    (!_stricmp(language,"ko") || !_stricmp(language,"ko-KR")) &&
                    pr_attribute(tag,(size_t)(close-tag),"href",href,sizeof(href)) && pr_link_url(response->url,href,url,sizeof(url)) &&
                    pr_parse_url(response->url,0,&from) && pr_parse_url(url,0,&to) && !strcmp(from.origin,to.origin)) pr_copy(page->ko_url,sizeof(page->ko_url),url);
            }
            if(!strcmp(name,"a")) {
                if(!ending && pr_attribute(tag,(size_t)(close-tag),"href",anchor,sizeof(anchor))) {
                    memset(&active,0,sizeof(active)); in_anchor=pr_link_url(response->url,anchor,active.url,sizeof(active.url));
                } else if(ending && in_anchor) {
                    pr_keep_owner_link(page,&active,response->url);
                    if(page->link_count<12) page->links[page->link_count++]=active;
                    in_anchor=0;
                }
            }
            if(!strcmp(name,"p") || !strcmp(name,"br") || !strcmp(name,"div") || !strcmp(name,"h1") || !strcmp(name,"h2") || !strcmp(name,"li")) pr_append(page->text,sizeof(page->text),"\n");
            cursor=close+1;
        } else {
            const char *next=memchr(cursor,'<',(size_t)(end-cursor)); char text[4096];
            if(!next) next=end;
            if(!skip[0]) {
                pr_plain(cursor,(size_t)(next-cursor),text,sizeof(text));
                if(in_title && !page->title[0]) pr_append(page->title,sizeof(page->title),text);
                if(in_anchor) pr_append(active.text,sizeof(active.text),text);
                if(footer || pr_find_ci(text,"copyright") || strstr(text,"\xc2\xa9")) { pr_append(page->legal,sizeof(page->legal)," "); pr_append(page->legal,sizeof(page->legal),text); }
                else { pr_append(page->text,sizeof(page->text)," "); pr_append(page->text,sizeof(page->text),text); }
            }
            cursor=next;
        }
    }
    return page->title[0] || page->description[0] || page->text[0];
}
#endif
