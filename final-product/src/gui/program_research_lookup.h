/* 공개 자료 조회의 진행 순서와 근거 채택 규칙이다. program_research_plan은 전송 전
 * 공개 항목/URL 미리보기만 만들고, lookup은 동의 확인 뒤 서명 주소→Wikidata 후보→
 * 실제 문서를 읽는다. 공식 제품 연결은 pr_try_page, 직접 역할 문장은 pr_evidence에서 바꾼다. */
#ifndef PROGRAM_RESEARCH_LOOKUP_H
#define PROGRAM_RESEARCH_LOOKUP_H
static int pr_public_label(const char *text,size_t cap,int required)
{
    size_t length; wchar_t wide[1024]; const unsigned char *p=(const unsigned char *)text;
    if(!memchr(text,0,cap)) return 0; length=strlen(text);
    if(!length) return !required;
    if(length>=cap || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,wide,1024)) return 0;
    while(*p) {
        if(*p<32 || *p==127 || strchr("\\/:@?#%<>|`\"",*p)) return 0;
        ++p;
    }
    return 1;
}
static int pr_escape_query(const char *text,char *out,size_t cap)
{
    const unsigned char *p=(const unsigned char *)text; size_t used=0;
    static const char hex[]="0123456789ABCDEF";
    while(*p) {
        if(isalnum(*p) || *p=='-' || *p=='.' || *p=='_' || *p=='~') { if(used+1>=cap) return 0; out[used++]=(char)*p; }
        else { if(used+3>=cap) return 0; out[used++]='%'; out[used++]=hex[*p>>4]; out[used++]=hex[*p&15]; }
        ++p;
    }
    out[used]=0; return 1;
}
static int pr_qid(const char *id)
{
    size_t i;
    if(id[0]!='Q' || !id[1] || strlen(id)>15) return 0;
    for(i=1;id[i];++i) if(id[i]<'0' || id[i]>'9') return 0;
    return 1;
}
/* 입력 검증과 공개 전송 계획만 만든다. 제품명만 검색 query에 넣고 게시자 등은 로컬
 * 대조용으로 남긴다. 1은 네트워크 조회 성공이 아니라 전송 가능한 계획이라는 뜻이다. */
static int program_research_plan(const ProgramResearchRequest *request,ProgramResearchPlan *plan)
{
    char encoded[1024]; PrUrl home;
    if(!plan) return 0; memset(plan,0,sizeof(*plan));
    if(!request || !request->local_identity_verified ||
        !pr_public_label(request->product,sizeof(request->product),1) ||
        !pr_public_label(request->publisher,sizeof(request->publisher),1) ||
        !pr_public_label(request->company,sizeof(request->company),0) ||
        !pr_public_label(request->description,sizeof(request->description),0) ||
        !pr_public_label(request->original,sizeof(request->original),0) ||
        !pr_escape_query(request->product,encoded,sizeof(encoded))) return 0;
    if(request->homepage_source!=PROGRAM_RESEARCH_HOME_NONE) {
        if(request->homepage_source>PROGRAM_RESEARCH_HOME_SIGNED_VERSION ||
            !memchr(request->homepage_origin,0,sizeof(request->homepage_origin)) ||
            !pr_parse_url(request->homepage_origin,0,&home) || strcmp(home.origin,request->homepage_origin)) return 0;
        pr_copy(plan->homepage_origin,sizeof(plan->homepage_origin),home.origin);
    }
    snprintf(plan->wikidata_search_url,sizeof(plan->wikidata_search_url),
        "https://www.wikidata.org/w/api.php?action=wbsearchentities&search=%s&language=en&uselang=en&type=item&limit=5&format=json",encoded);
    snprintf(plan->disclosure_ko,sizeof(plan->disclosure_ko),
        "조사 순서: %s확정하지 못하면 Wikidata에서 제품명만 검색하고 공개 제품·게시자 관계를 확인한 뒤 발견한 공개 HTTPS 문서를 읽습니다. 후속 Wikidata 요청에는 공개 Q-ID만 사용합니다. 게시자·회사명·설명·원래 파일명·목적은 검색 query에 보내지 않고 로컬 대조에만 사용합니다. 문서 서버에는 요청 URL·익명 HTTP 헤더와 접속 IP가 전달되며 쿠키·로그인·로컬 경로·PID·계정 정보는 보내지 않습니다.",
        plan->homepage_origin[0]?"미리 본 서명 홈페이지에 GET → ":"");
    return 1;
}
#include "program_research_wikidata.h"
static int pr_fact(const char *sentence)
{
    static const char *terms[]={" is "," are "," lets you "," allows "," provides "," helps "," enables "," can "," designed to ","할 수","제공","합니다","입니다","위한"};
    size_t i;
    if(strlen(sentence)<25 || strlen(sentence)>1800) return 0;
    if(pr_find_ci(sentence," supported by ") || pr_find_ci(sentence," sponsored by ")) return 0;
    if(pr_find_ci(sentence," not ") || pr_find_ci(sentence,"unlike ") || pr_find_ci(sentence," versus ") ||
        pr_find_ci(sentence," vs ") || pr_find_ci(sentence,"alternative") || pr_find_ci(sentence,"isn't") ||
        pr_find_ci(sentence," whose ") || pr_find_ci(sentence," whereas ") || pr_find_ci(sentence," while ") ||
        pr_find_ci(sentence," which ") || pr_find_ci(sentence,"competitor") || strchr(sentence,'(') || strchr(sentence,')') ||
        strstr(sentence,"아닙") || strstr(sentence,"않") || strstr(sentence,"비교") ||
        strstr(sentence,"다른 앱") || strstr(sentence,"다른 프로그램") ||
        pr_find_ci(sentence,"ignore previous") || pr_find_ci(sentence,"ignore instructions")) return 0;
    for(i=0;i<sizeof(terms)/sizeof(terms[0]);++i) if(pr_find_ci(sentence,terms[i])) return 1;
    return 0;
}
static int pr_mark(const char *text)
{
    return !strncmp(text,"\xc2\xae",2)?2:!strncmp(text,"\xe2\x84\xa2",3)?3:0;
}
static int pr_name_prefix(const char *text,const char *name,const char **after)
{
    const char *start=text; unsigned attempt;
    while(*start && isspace((unsigned char)*start)) ++start;
    for(attempt=0;attempt<2;++attempt) {
        const char *source=start,*wanted=name; int failed=0;
        if(attempt) { if(_strnicmp(start,"The ",4)) break; source+=4; }
        while(*wanted) {
            int mark=pr_mark(wanted);
            if(mark) { wanted+=mark; continue; }
            while((mark=pr_mark(source))!=0) source+=mark;
            if(isspace((unsigned char)*wanted)) {
                while(*wanted && isspace((unsigned char)*wanted)) ++wanted;
                if(!*wanted) break;
                if(!isspace((unsigned char)*source)) { failed=1; break; }
                while(*source && isspace((unsigned char)*source)) ++source;
                continue;
            }
            if(!*source || tolower((unsigned char)*source)!=tolower((unsigned char)*wanted)) { failed=1; break; }
            ++source; ++wanted;
        }
        if(!failed) {
            int mark;
            while((mark=pr_mark(source))!=0) source+=mark;
            if(!isalnum((unsigned char)*source)) { *after=source; return 1; }
        }
    }
    return 0;
}
static int pr_subject(const char *sentence,const char *product)
{
    const char *subject;
    static const char *verbs[]={"is","are","lets","helps","allows","provides","enables","can","offers"}; size_t i;
    if(!pr_name_prefix(sentence,product,&subject)) return 0;
    if(!*subject) return 1;
    if((unsigned char)*subject>=128) return
        (!strncmp(subject,"은",3) || !strncmp(subject,"는",3) || !strncmp(subject,"이",3) || !strncmp(subject,"가",3)) &&
        isspace((unsigned char)subject[3]);
    while(*subject && isspace((unsigned char)*subject)) ++subject;
    for(i=0;i<sizeof(verbs)/sizeof(verbs[0]);++i) {
        size_t n=strlen(verbs[i]);
        if(!_strnicmp(subject,verbs[i],n) && isspace((unsigned char)subject[n])) return 1;
    }
    return 0;
}
/* 본문/문서 소개에서 직접 기능을 설명하는 문장을 최대 세 개 복사한다. 후보 페이지의
 * 주장일 뿐 현재 인스턴스의 작업/안전을 판정하지 않는다. 문장 선택·제외 규칙은 pr_fact와
 * pr_subject를 함께 수정하고, 한국어 고정 구절 대응은 official_role_summary.h에서 다룬다. */
static void pr_evidence(ProgramResearchResult *result,const char *text,int require_product)
{
    const char *cursor=text;
    while(*cursor && result->evidence_count<3) {
        const char *end=cursor; char sentence[2048]; unsigned i;
        while(*end && *end!='\n' && *end!=';' && *end!='!' && *end!='?' && !(*end=='.' && (end[1]==' ' || !end[1]))) ++end;
        if(*end && *end!='\n') ++end;
        if((size_t)(end-cursor)<sizeof(sentence)) {
            pr_plain(cursor,(size_t)(end-cursor),sentence,sizeof(sentence));
            if(pr_fact(sentence) && (!require_product || pr_subject(sentence,result->matched_product[0]?result->matched_product:result->request.product))) {
                for(i=0;i<result->evidence_count;++i) if(!strcmp(result->evidence[i],sentence)) break;
                if(i==result->evidence_count) pr_copy(result->evidence[result->evidence_count++],sizeof(result->evidence[0]),sentence);
            }
        }
        cursor=end; while(*cursor && isspace((unsigned char)*cursor)) ++cursor;
    }
}
static int pr_actor_prefix(const char *text,const char *actor)
{
    char prefix[512]; size_t i,n=strlen(text);
    const char *opening,*closing;
    if(n>=sizeof(prefix)) n=sizeof(prefix)-1;
    memcpy(prefix,text,n); prefix[n]=0;
    for(i=0;i<n;++i) if(prefix[i]==',' || prefix[i]==';' || prefix[i]=='(' || prefix[i]=='\n') { prefix[i]=0; break; }
    if(pr_identity_equal(prefix,actor,1)) return 1;
    opening=strchr(text,'('); closing=opening?strchr(opening+1,')'):NULL;
    if(opening && closing && (size_t)(opening-text)<=80 && (size_t)(closing-opening)<sizeof(prefix)) {
        size_t words=0; int in_word=0;
        if(_strnicmp(text,"a ",2) && _strnicmp(text,"an ",3) && _strnicmp(text,"the ",4)) return 0;
        for(i=0;text+i<opening;++i) {
            unsigned char c=(unsigned char)text[i];
            if(c>='a' && c<='z') { if(!in_word) ++words; in_word=1; }
            else if(c==' ' || c=='-') in_word=0; else return 0;
        }
        if(words<2 || words>10) return 0;
        n=(size_t)(closing-opening-1); memcpy(prefix,opening+1,n); prefix[n]=0;
        if(!pr_identity_equal(prefix,actor,1)) return 0;
        text=closing+1; while(*text==' ') ++text;
        if(!*text || (*text=='.' && !text[1])) return 1;
        if(strncmp(text,"and ",4)) return 0;
        text+=4;
        if(strlen(text)<160 && !strchr(text,'(') && !strchr(text,')') &&
            (pr_find_ci(text," by ") || pr_find_ci(text," is ") || pr_find_ci(text," are ") ||
            pr_find_ci(text," has ") || pr_find_ci(text," can ") || pr_find_ci(text," does "))) return 1;
        if(strncmp(text,"a ",2) && strncmp(text,"an ",3) && strncmp(text,"the ",4)) return 0;
        for(i=0;text[i];++i) if(!((text[i]>='a' && text[i]<='z') || text[i]==' ' || text[i]=='-' || (text[i]=='.' && !text[i+1]))) return 0;
        return i>2 && i<160;
    }
    return 0;
}
static int pr_owner_product_subject(const char *before,const char *product)
{
    const char *text; 
    static const char *allowed[]={"is","currently","actively","jointly","developed","maintained","distributed","created","written","authored","and","led"};
    if(!pr_name_prefix(before,product,&text) || !isspace((unsigned char)*text)) return 0;
    while(*text && isspace((unsigned char)*text)) ++text;
    if(_strnicmp(text,"is",2) || (text[2] && !isspace((unsigned char)text[2]))) return 0;
    while(*text) {
        size_t n=0,i; int matched=0;
        while(*text && isspace((unsigned char)*text)) ++text;
        while(text[n] && !isspace((unsigned char)text[n])) ++n;
        if(!n) break;
        for(i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i) if(strlen(allowed[i])==n && !_strnicmp(text,allowed[i],n)) matched=1;
        if(!matched) return 0; text+=n;
    }
    return 1;
}
static int pr_owner_signer_subject(const char *before,const char *signer)
{
    const char *is=pr_find_ci(before," is "); char actor[512]; size_t n,i;
    static const char *modifiers[]={"the","a","an","lead","principal","current","primary","main","project","founding","co-founder","co-maintainer"};
    if(!is || (size_t)(is-before)>=sizeof(actor)) return 0;
    n=(size_t)(is-before); memcpy(actor,before,n); actor[n]=0;
    if(!pr_identity_equal(actor,signer,1)) return 0;
    before=is+4;
    while(*before) {
        int matched=0;
        while(*before && isspace((unsigned char)*before)) ++before;
        for(n=0;before[n] && !isspace((unsigned char)before[n]);++n) {}
        if(!n) break;
        for(i=0;i<sizeof(modifiers)/sizeof(modifiers[0]);++i) if(strlen(modifiers[i])==n && !_strnicmp(before,modifiers[i],n)) matched=1;
        if(!matched) return 0; before+=n;
    }
    return 1;
}
static int pr_owner_sentence(const char *sentence,const char *product,const char *signer)
{
    static const char *by[]={"developed by ","maintained by ","distributed by ","created by ","written by ","led by ","authored by ","lead developer: ","maintainer: "};
    static const char *of[]={"developer of ","maintainer of ","creator of ","founder of ","project leader of ","lead developer of "};
    size_t i;
    if(!pr_mentions(sentence,product,0) || !pr_mentions(sentence,signer,1) || strlen(sentence)>900 ||
        pr_find_ci(sentence,"including ") || pr_find_ci(sentence,"sponsor") || pr_find_ci(sentence,"thanks ") ||
        pr_find_ci(sentence," not ") || pr_find_ci(sentence,"former ") || pr_find_ci(sentence,"isn't") ||
        pr_find_ci(sentence,"wasn't") || pr_find_ci(sentence,"no longer") || pr_find_ci(sentence,"alternative") ||
        pr_find_ci(sentence," versus ") || pr_find_ci(sentence," vs ") || strstr(sentence,"않") || strstr(sentence,"아닙")) return 0;
    for(i=0;i<sizeof(by)/sizeof(by[0]);++i) {
        const char *relation=pr_find_ci(sentence,by[i]);
        if(relation) {
            char before[1024],after[1024]; size_t n=(size_t)(relation-sentence);
            if(n>=sizeof(before)) continue; memcpy(before,sentence,n); before[n]=0;
            pr_copy(after,sizeof(after),relation+strlen(by[i]));
            n=strlen(after); while(n && (after[n-1]=='.' || after[n-1]==' ')) after[--n]=0;
            if(pr_owner_product_subject(before,product) && pr_actor_prefix(after,signer)) return 1;
        }
    }
    for(i=0;i<sizeof(of)/sizeof(of[0]);++i) {
        const char *relation=pr_find_ci(sentence,of[i]);
        if(relation) {
            char before[1024]; size_t n=(size_t)(relation-sentence);
            if(n>=sizeof(before)) continue; memcpy(before,sentence,n); before[n]=0;
            if(pr_owner_signer_subject(before,signer) && pr_mentions(relation+strlen(of[i]),product,0)) return 1;
        }
    }
    return 0;
}
static int pr_owner_quote(const PrPage *page,const char *product,const char *signer,char *out,size_t cap)
{
    const char *cursor=page->text;
    while(*cursor) {
        const char *end=cursor; char sentence[1024];
        while(*end && *end!='\n' && !(*end=='.' && (!end[1] || end[1]==' '))) ++end;
        if(*end=='.') ++end;
        if((size_t)(end-cursor)<sizeof(sentence)) {
            pr_plain(cursor,(size_t)(end-cursor),sentence,sizeof(sentence));
            if(pr_owner_sentence(sentence,product,signer)) return pr_copy(out,cap,sentence);
        }
        cursor=end; while(*cursor && isspace((unsigned char)*cursor)) ++cursor;
    }
    return 0;
}
static void pr_clear_source(ProgramResearchResult *result)
{
    unsigned i;
    result->official_confirmed=result->product_matched=0;
    result->evidence_count=result->evidence_meta_mask=0;
    result->url[0]=result->title[0]=result->origin[0]=result->discovery_url[0]=result->authority_text[0]=0;
    result->matched_product[0]=result->ownership_url[0]=result->ownership_evidence[0]=result->page_text[0]=0;
    for(i=0;i<3;++i) result->evidence[i][0]=0;
}
static int pr_store_owner(ProgramResearchResult *result,const char *url,const char *quote)
{
    if(strlen(url)>=sizeof(result->ownership_url) || strlen(quote)>=sizeof(result->ownership_evidence)) {
        pr_clear_source(result); result->error=ERROR_BUFFER_OVERFLOW; return 0;
    }
    pr_copy(result->ownership_url,sizeof(result->ownership_url),url);
    pr_copy(result->ownership_evidence,sizeof(result->ownership_evidence),quote);
    return 1;
}
static int pr_store_source(ProgramResearchResult *result,const char *url,const char *origin,const char *title,
    const char *text,const char *product,const char *discovery)
{
    if(strlen(url)>=sizeof(result->url) || strlen(origin)>=sizeof(result->origin) ||
        strlen(title)>=sizeof(result->title) || strlen(text)>=sizeof(result->page_text) ||
        strlen(product)>=sizeof(result->matched_product) || strlen(discovery)>=sizeof(result->discovery_url)) {
        pr_clear_source(result); result->error=ERROR_BUFFER_OVERFLOW; return 0;
    }
    pr_copy(result->url,sizeof(result->url),url); pr_copy(result->origin,sizeof(result->origin),origin);
    pr_copy(result->title,sizeof(result->title),title); pr_copy(result->page_text,sizeof(result->page_text),text);
    pr_copy(result->matched_product,sizeof(result->matched_product),product);
    pr_copy(result->discovery_url,sizeof(result->discovery_url),discovery);
    return 1;
}
static int pr_owner_lookup(PrNet *net,const PrResponse *landing,const PrPage *page,const char *product)
{
    unsigned i; PrUrl origin; char quote[1024];
    if(!pr_parse_url(landing->url,0,&origin)) return 0;
    if(pr_owner_quote(page,product,net->result->request.publisher,quote,sizeof(quote)))
        return pr_store_owner(net->result,landing->url,quote);
    for(i=0;i<page->owner_link_count && i<2 && net->result->request_count<PROGRAM_RESEARCH_REQUEST_LIMIT;++i) {
        PrResponse response={0}; PrPage *owner=(PrPage *)calloc(1,sizeof(*owner)); PrUrl final_url;
        int found=0;
        if(!owner) return 0;
        if(strcmp(page->owner_links[i].url,landing->url) && pr_fetch(net,page->owner_links[i].url,0,&response) &&
            pr_parse_url(response.url,0,&final_url) && pr_same_web_host(&origin,&final_url) &&
            pr_html(&response,owner) && pr_owner_quote(owner,product,net->result->request.publisher,
                quote,sizeof(quote)))
            found=pr_store_owner(net->result,response.url,quote);
        pr_response_free(&response); free(owner); if(found) return 1;
    }
    return 0;
}
/* 이전 페이지 근거를 지운 뒤 실제 문서의 제품 제목과 게시자 법적 표기/관계/운영주체를
 * 대조한다. 인용까지 확보해야 official_confirmed를 세운다. 실패해도 일부 값이 남을 수
 * 있으므로 호출자는 문자열 존재만으로 공식 자료로 취급하지 않는다. response는 여기서 해제한다. */
static int pr_try_page(PrNet *net,const char *url,const PrDiscovery *discovery,int signed_hint,PrPage *page)
{
    PrResponse response={0}; PrUrl parsed,publisher,seed; int product_title,legal,peer=0,owner=0;
    const char *product=net->result->request.product;
    pr_clear_source(net->result);
    if(!pr_fetch(net,url,0,&response)) return 0;
    if(!strstr(response.content_type,"text/html") || !pr_html(&response,page) || !pr_parse_url(response.url,0,&parsed)) { pr_response_free(&response); return 0; }
    if(net->result->request.description[0] && pr_identity_equal(page->title,net->result->request.description,0)) product=net->result->request.description;
    product_title=pr_mentions(page->title,product,0);
    legal=pr_mentions(page->legal,net->result->request.publisher,1);
    if(discovery && discovery->relation_verified && discovery->publisher_url[0] && pr_parse_url(discovery->publisher_url,0,&publisher))
        peer=!wcscmp(publisher.path,L"/") && !strcmp(publisher.origin,parsed.origin);
    if(product_title && !legal && !peer && (signed_hint || (discovery && discovery->relation_verified))) owner=pr_owner_lookup(net,&response,page,product);
    /* 서명 링크 하나/데이터베이스의 주소 하나만으로 official을 단정하지 않는다. */
    if(!product_title || !((discovery && discovery->relation_verified && (legal || peer || owner)) ||
        (signed_hint && (legal || owner) && pr_parse_url(net->result->request.homepage_origin,0,&seed) && !strcmp(seed.origin,parsed.origin)))) {
        pr_response_free(&response); return 0;
    }
    /* 같은 확인 origin의 한국어 대체 문서를 먼저 읽는다. */
    if(page->ko_url[0] && strcmp(page->ko_url,response.url) && net->result->request_count<PROGRAM_RESEARCH_REQUEST_LIMIT-1) {
        PrResponse korean={0}; PrPage *localized=(PrPage *)calloc(1,sizeof(*localized));
        if(localized && pr_fetch(net,page->ko_url,0,&korean) && pr_html(&korean,localized) &&
            pr_mentions(localized->title,product,0)) {
            pr_response_free(&response); response=korean; memset(&korean,0,sizeof(korean)); *page=*localized;
            pr_parse_url(response.url,0,&parsed);
        }
        pr_response_free(&korean); free(localized);
    }
    if(!pr_store_source(net->result,response.url,parsed.origin,page->title,page->text,product,discovery?discovery->entity_url:"")) {
        pr_response_free(&response); return 0;
    }
    net->result->evidence_count=net->result->evidence_meta_mask=0;
    pr_evidence(net->result,page->text,1);
    {
        unsigned body_count=net->result->evidence_count,i;
        pr_evidence(net->result,page->description,1);
        for(i=body_count;i<net->result->evidence_count;++i) net->result->evidence_meta_mask|=1u<<i;
    }
    if(!net->result->evidence_count) { pr_response_free(&response); return 0; }
    net->result->official_confirmed=net->result->product_matched=1;
    snprintf(net->result->authority_text,sizeof(net->result->authority_text),
        "%s 제품명과 실제 제작사 문서의 제품 설명을 대조했습니다. %s. 공개 자료는 현재 인스턴스의 작업·안전·필요성을 증명하지 않습니다.",
        discovery && discovery->relation_verified?"Wikidata의 정확한 제품·개발사/게시자 관계와":"검증된 서명자가 제공한 홈페이지 단서의",
        legal?"문서의 게시자 법적 표기도 일치합니다":owner?"직접 연결한 운영주체 문장의 제품·서명자 관계도 일치합니다":"게시자 조직의 별도 공식 주소 주장과 문서 origin이 일치합니다");
    if(owner) {
        pr_append(net->result->authority_text,sizeof(net->result->authority_text)," 운영주체 근거: ");
        pr_append(net->result->authority_text,sizeof(net->result->authority_text),net->result->ownership_evidence);
        pr_append(net->result->authority_text,sizeof(net->result->authority_text)," · ");
        pr_append(net->result->authority_text,sizeof(net->result->authority_text),net->result->ownership_url);
    }
    if(net->result->evidence_meta_mask==((1u<<net->result->evidence_count)-1u))
        pr_append(net->result->authority_text,sizeof(net->result->authority_text)," 인용은 본문이 아닌 HTML 문서 소개(meta description)에서 가져왔습니다.");
    pr_response_free(&response); return 1;
}
/* worker가 호출하는 조회 진입점이다. 요청을 복사하고 제한된 세션/횟수로 문서를 읽는다.
 * 미동의는 NOT_REQUESTED, 확정 근거 부족은 UNKNOWN, 오류는 FAILED, 취소는 CANCELLED로
 * 남긴다. 실패를 '제품 정보 없음'으로 바꾸거나 자동 재시도하지 않는다. 세션/임시 본문은
 * 여기서 정리하고 결과의 복사 문자열과 기록 UTC·유효기간만 호출자에게 남긴다. */
static void program_research_lookup(const ProgramResearchRequest *request,ProgramResearchResult *result)
{
    PrNet net; WSADATA winsock; DWORD protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; BOOL no_credentials=FALSE;
    PrDiscovery discovery; PrPage *page=NULL; FILETIME now; PrUrl seed; unsigned i;
    ProgramResearchPlan plan;
    if(!result) return; memset(result,0,sizeof(*result)); result->status=PROGRAM_RESEARCH_NOT_REQUESTED;
    if(!request) return; result->request=*request;
    if(!request->user_approved_public) { pr_copy(result->failure_text,sizeof(result->failure_text),"공개 정보 전송에 동의한 뒤 조사를 시작할 수 있습니다."); return; }
    result->status=PROGRAM_RESEARCH_UNKNOWN;
    if(!program_research_plan(request,&plan)) {
        pr_copy(result->failure_text,sizeof(result->failure_text),"공개 제품·게시자 입력 또는 로컬 신원을 확인하지 못해 외부 조사를 생략했습니다."); return;
    }
    if(!result->request.deadline_tick) result->request.deadline_tick=GetTickCount64()+30000;
    if(pr_cancelled(&result->request)) { result->status=PROGRAM_RESEARCH_CANCELLED; return; }
    if(WSAStartup(MAKEWORD(2,2),&winsock)) { result->status=PROGRAM_RESEARCH_FAILED; return; }
    net.result=result; net.session=WinHttpOpen(L"PC-Resource-Monitor/0.1 (anonymous public document lookup)",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!net.session) { result->error=GetLastError(); result->status=PROGRAM_RESEARCH_FAILED; WSACleanup(); return; }
    if(!WinHttpSetTimeouts(net.session,3000,3000,3000,3000) ||
        !WinHttpSetOption(net.session,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols)) ||
        !WinHttpSetOption(net.session,WINHTTP_OPTION_USE_GLOBAL_SERVER_CREDENTIALS,&no_credentials,sizeof(no_credentials))) {
        result->error=GetLastError(); result->status=PROGRAM_RESEARCH_FAILED; WinHttpCloseHandle(net.session); WSACleanup(); return;
    }
    page=(PrPage *)calloc(1,sizeof(*page)); memset(&discovery,0,sizeof(discovery));
    if(!page) { result->error=ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if(request->homepage_source!=PROGRAM_RESEARCH_HOME_NONE && pr_parse_url(request->homepage_origin,0,&seed) && !strcmp(seed.origin,request->homepage_origin)) {
        if(pr_try_page(&net,seed.url,NULL,1,page)) goto complete;
        for(i=0;i<page->link_count && result->request_count<4;++i) {
            PrUrl link;
            if(pr_mentions(page->links[i].text,request->product,0) && pr_parse_url(page->links[i].url,0,&link) && !strcmp(link.origin,seed.origin)) {
                PrPage *child=(PrPage *)calloc(1,sizeof(*child));
                if(child && pr_try_page(&net,link.url,NULL,1,child)) { free(child); goto complete; }
                free(child);
            }
        }
    }
    if(pr_discover(&net,&discovery)) {
        if(pr_try_page(&net,discovery.product_url,&discovery,0,page)) goto complete;
        if(discovery.publisher_url[0] && pr_try_page(&net,discovery.publisher_url,&discovery,0,page)) goto complete;
    }
    if(!result->failure_text[0]) {
        if(result->error) snprintf(result->failure_text,sizeof(result->failure_text),"문서 조회를 완료하지 못했습니다(오류 %lu, HTTP %lu). 제품 정보 없음으로 해석하지 않으며 자동 재시도하지 않습니다.",(unsigned long)result->error,(unsigned long)result->http_status);
        else pr_copy(result->failure_text,sizeof(result->failure_text),"현재 공식 자료를 확정하지 못했습니다. 제품·게시자 관계, 현재 URL 또는 실제 기능 문장이 부족합니다. 이름과 자원 사용량으로 역할을 추정하지 않습니다.");
    }
    goto done;
complete:
    result->status=PROGRAM_RESEARCH_COMPLETE; result->error=0; result->failure_text[0]=0;
    pr_copy(result->summary_ko,sizeof(result->summary_ko),"제작사 공개 문서에서 확인한 역할 원문입니다. 현재 프로세스가 그 작업을 수행 중이라는 뜻은 아닙니다.");
    GetSystemTimeAsFileTime(&now); result->fetched_utc=((uint64_t)now.dwHighDateTime<<32)|now.dwLowDateTime;
    result->checked_tick=GetTickCount64(); result->expires_tick=result->checked_tick+PROGRAM_RESEARCH_LIFETIME_MS;
done:
    if(result->status!=PROGRAM_RESEARCH_COMPLETE && result->error) result->status=PROGRAM_RESEARCH_FAILED;
    if(pr_cancelled(&result->request)) { result->status=PROGRAM_RESEARCH_CANCELLED; result->official_confirmed=0; result->evidence_count=0; }
    free(page); WinHttpCloseHandle(net.session); WSACleanup();
}
#endif
