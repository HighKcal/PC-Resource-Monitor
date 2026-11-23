/* 공개 제품명으로 Wikidata 후보를 발견하고 로컬 서명 게시자와 관계를 대조한다.
 * pr_discover는 실제 문서를 읽을 주소 단서만 반환한다. 여기의 분류·관계는 외부 데이터이며
 * 현재 프로그램의 역할/안전 판정이 아니다. 최종 제품 문서 채택은 lookup의 pr_try_page가 맡는다. */
#ifndef PROGRAM_RESEARCH_WIKIDATA_H
#define PROGRAM_RESEARCH_WIKIDATA_H
#define PR_WD_CANDIDATE_LIMIT 5u
#define PR_WD_PERSON_LIMIT 3u
#define PR_WD_PERSON_BATCH_LIMIT (PR_WD_CANDIDATE_LIMIT * PR_WD_PERSON_LIMIT)
#ifndef PROGRAM_RESEARCH_WD_FETCH
#define PROGRAM_RESEARCH_WD_FETCH pr_fetch
#endif
static int pr_wd_raw_equal(const char *a,const char *b)
{
    wchar_t aa[512],bb[512];char left[512],right[512];
    const char *inputs[2]={a,b};char *outputs[2]={left,right};
    for(unsigned i=0;i<2;++i) {
        const char *start=inputs[i],*end;size_t n;
        if(!start)return 0;while(*start && isspace((unsigned char)*start))++start;
        end=start+strlen(start);while(end>start && isspace((unsigned char)end[-1]))--end;
        n=(size_t)(end-start);if(!n || n>=512)return 0;memcpy(outputs[i],start,n);outputs[i][n]=0;
    }
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,left,-1,aa,(int)_countof(aa)) &&
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,right,-1,bb,(int)_countof(bb)) && CompareStringOrdinal(aa,-1,bb,-1,TRUE)==CSTR_EQUAL;
}
/* 4는 원문 주 이름, 3은 원문 별칭, 2는 정규화 이름, 1은 정규화 별칭 일치다.
 * 제품 후보 선택은 3/4만 허용해 구두점을 제거해야 같아지는 이름으로 고르지 않는다. */
static int pr_wd_name_quality(const PrJson *json,int entity,const char *wanted,int company)
{
    int labels=pr_json_field(json,entity,"labels"),aliases=pr_json_field(json,entity,"aliases");
    int item,quality=0;
    if(labels>=0 && json->tokens[labels].type=='{') for(item=labels+1;item<json->tokens[labels].next;) {
        int value=item+1; char label[512];
        if(pr_json_string(json,pr_json_field(json,value,"value"),label,sizeof(label))) {
            if(pr_wd_raw_equal(label,wanted))return 4;
            if(pr_identity_equal(label,wanted,company))quality=2;
        }
        item=json->tokens[value].next;
    }
    if(aliases>=0 && json->tokens[aliases].type=='{') for(item=aliases+1;item<json->tokens[aliases].next;) {
        int array=item+1,entry;
        if(json->tokens[array].type=='[') for(entry=array+1;entry<json->tokens[array].next;entry=json->tokens[entry].next) {
            char alias[512];
            if(pr_json_string(json,pr_json_field(json,entry,"value"),alias,sizeof(alias))) {
                if(pr_wd_raw_equal(alias,wanted)){if(quality<3)quality=3;}
                else if(!quality && pr_identity_equal(alias,wanted,company))quality=1;
            }
        }
        item=json->tokens[array].next;
    }
    return quality;
}
static int pr_entity_name(const PrJson *json,int entity,const char *wanted,int company) { return pr_wd_name_quality(json,entity,wanted,company)>0; }
static int pr_wd_snak_value(const PrJson *json,int snak)
{
    if(snak<0 || !pr_json_equal(json,pr_json_field(json,snak,"snaktype"),"value"))return -1;
    return pr_json_field(json,pr_json_field(json,snak,"datavalue"),"value");
}
static int pr_wd_started(const PrJson *json,int snak)
{
    int value=pr_wd_snak_value(json,snak);char time[80],calendar[128],*end;
    unsigned long year;unsigned month,day,hour,minute,second,maxday;SYSTEMTIME now;
    if(!pr_json_string(json,pr_json_field(json,value,"time"),time,sizeof(time)) || time[0]!='+')return 0;
    if(pr_json_field(json,value,"calendarmodel")>=0 &&
        (!pr_json_string(json,pr_json_field(json,value,"calendarmodel"),calendar,sizeof(calendar)) ||
        (strcmp(calendar,"http://www.wikidata.org/entity/Q1985727") && strcmp(calendar,"https://www.wikidata.org/entity/Q1985727"))))return 0;
    year=strtoul(time+1,&end,10);
    if(end==time+1 || !year || year>9999 || *end!='-' || strlen(end)!=16 || end[3]!='-' || end[6]!='T' || end[9]!=':' || end[12]!=':' || end[15]!='Z')return 0;
    for(const char *p=time+1;p<end;++p)if(!isdigit((unsigned char)*p))return 0;
    for(unsigned i=1;i<15;++i)if(i!=3 && i!=6 && i!=9 && i!=12 && !isdigit((unsigned char)end[i]))return 0;
    month=(unsigned)(end[1]-'0')*10u+(unsigned)(end[2]-'0');day=(unsigned)(end[4]-'0')*10u+(unsigned)(end[5]-'0');
    hour=(unsigned)(end[7]-'0')*10u+(unsigned)(end[8]-'0');minute=(unsigned)(end[10]-'0')*10u+(unsigned)(end[11]-'0');second=(unsigned)(end[13]-'0')*10u+(unsigned)(end[14]-'0');
    if(month>12 || day>31 || (!month && day) || hour>23 || minute>59 || second>59)return 0;
    maxday=month==2?((year%4==0 && (year%100!=0 || year%400==0))?29u:28u):(month==4 || month==6 || month==9 || month==11)?30u:31u;
    if(day>maxday)return 0;GetSystemTime(&now);
    if(year!=now.wYear)return year<now.wYear;if(!month)return 0;if(month!=now.wMonth)return month<now.wMonth;
    if(!day)return 0;if(day!=now.wDay)return day<now.wDay;
    return hour<now.wHour || (hour==now.wHour && (minute<now.wMinute || (minute==now.wMinute && second<=now.wSecond)));
}
/* 외부 주장의 rank는 normal=1, preferred=2로 반환하고 제외하면 0이다.
 * 종료(P582)·특정 시점(P585) 한정은 존재만으로 제외한다. 시작(P580)이 없으면
 * 시작 시각 검사를 생략하며, 있으면 1~8개 값 모두 pr_wd_started로 해석돼야 한다.
 * 완전한 시각은 현재 UTC와 같은 초까지 허용한다. 월 생략은 지난 연도, 일 생략은
 * 지난 연도 또는 지난 달인 경우만 허용한다.
 * 이것은 조사 시점의 후보 선택 규칙이며 실제 제품 상태를 직접 관측한 결과가 아니다. */
static int pr_wd_claim_rank(const PrJson *json,int claim)
{
    int qualifiers=pr_json_field(json,claim,"qualifiers"),start;char rank[32];
    if(!pr_json_string(json,pr_json_field(json,claim,"rank"),rank,sizeof(rank)) || (strcmp(rank,"normal") && strcmp(rank,"preferred")))return 0;
    if(qualifiers>=0 && (pr_json_field(json,qualifiers,"P582")>=0 || pr_json_field(json,qualifiers,"P585")>=0))return 0;
    start=pr_json_field(json,qualifiers,"P580");
    if(start>=0) {unsigned count=0;if(json->tokens[start].type!='[' || start+1>=json->tokens[start].next)return 0;
        for(int item=start+1;item<json->tokens[start].next;item=json->tokens[item].next)if(++count>8 || !pr_wd_started(json,item))return 0;}
    return !strcmp(rank,"preferred")?2:1;
}
static int pr_current_claim(const PrJson *json,int claim)
{
    return pr_wd_claim_rank(json,claim)!=0;
}
static int pr_claim_value(const PrJson *json,int claim)
{
    int snak=pr_json_field(json,claim,"mainsnak"),value;
    if(snak<0 || !pr_json_equal(json,pr_json_field(json,snak,"snaktype"),"value")) return -1;
    value=pr_json_field(json,snak,"datavalue"); return pr_json_field(json,value,"value");
}
/* 0 언어 중립, 1 다른 언어, 2 영어, 3 한국어다. -1은 언어 한정을 해석하지 못한 경우다. */
static int pr_wd_language(const PrJson *json,int claim)
{
    int qualifiers=pr_json_field(json,claim,"qualifiers"),array=pr_json_field(json,qualifiers,"P407"),language=1;unsigned count=0;
    if(array<0)return 0;if(json->tokens[array].type!='[' || array+1>=json->tokens[array].next)return -1;
    for(int item=array+1;item<json->tokens[array].next;item=json->tokens[item].next) {
        char id[16];int value=pr_wd_snak_value(json,item);
        if(++count>8 || !pr_json_string(json,pr_json_field(json,value,"id"),id,sizeof(id)) || !pr_qid(id))return -1;
        if(!strcmp(id,"Q9176"))language=3;else if(language<2 && !strcmp(id,"Q1860"))language=2;
    }
    return language;
}
typedef struct PrWdWebsite {char url[2048],origin[256];int language,neutral_root;} PrWdWebsite;
static int pr_claim_url(const PrJson *json,int claims,char *url,size_t cap)
{
    int array=pr_json_field(json,claims,"P856"),best_rank=0,ok=0,chosen=-1,best_language=-1;
    unsigned inspected=0,count=0,unqualified=0,best_count=0;PrWdWebsite *sites=NULL;
    if(!url || !cap)return 0;url[0]=0;if(array<0 || json->tokens[array].type!='[')return 0;
    for(int claim=array+1;claim<json->tokens[array].next;claim=json->tokens[claim].next) {
        int rank=pr_wd_claim_rank(json,claim);if(++inspected>64)return 0;if(rank>best_rank)best_rank=rank;
    }
    if(!best_rank)return 0;sites=(PrWdWebsite *)calloc(32,sizeof(*sites));if(!sites)return 0;
    for(int claim=array+1;claim<json->tokens[array].next;claim=json->tokens[claim].next) {
        char value[2048],https[2048];PrUrl parsed;int language;unsigned duplicate;
        if(pr_wd_claim_rank(json,claim)!=best_rank)continue;
        if(!pr_json_string(json,pr_claim_value(json,claim),value,sizeof(value)))goto done;
        if(!strncmp(value,"http://",7)) {if(strlen(value)+1>=sizeof(https))goto done;snprintf(https,sizeof(https),"https://%s",value+7);}
        else if(!pr_copy(https,sizeof(https),value))goto done;
        if(!pr_parse_url(https,0,&parsed))goto done;language=pr_wd_language(json,claim);
        for(duplicate=0;duplicate<count;++duplicate)if(!strcmp(sites[duplicate].url,parsed.url))break;
        if(duplicate<count) {if(language>sites[duplicate].language)sites[duplicate].language=language;
            if(!language && !wcscmp(parsed.path,L"/"))sites[duplicate].neutral_root=1;continue;}
        if(count>=32)goto done;
        pr_copy(sites[count].url,sizeof(sites[count].url),parsed.url);pr_copy(sites[count].origin,sizeof(sites[count].origin),parsed.origin);
        sites[count].language=language;sites[count].neutral_root=!language && !wcscmp(parsed.path,L"/");++count;
    }
    if(!count)goto done;if(count==1){ok=pr_copy(url,cap,sites[0].url);goto done;}
    /* 경로가 정확히 같은 주소에만 www 유무를 동등하게 취급한다. 언어별 주소는
     * origin이 하나여야 하며 모호한 여러 URL 중 임의로 하나를 고르지 않는다. */
    {PrUrl first;int equivalent=1;if(!pr_parse_url(sites[0].url,0,&first))goto done;
        for(unsigned i=1;i<count;++i){PrUrl other;if(!pr_parse_url(sites[i].url,0,&other) || !pr_same_web_host(&first,&other) || wcscmp(first.path,other.path))equivalent=0;}
        if(equivalent){ok=pr_copy(url,cap,sites[0].url);goto done;}}
    for(unsigned i=0;i<count;++i) {
        if(strcmp(sites[0].origin,sites[i].origin) || sites[i].language<0)goto done;
        if(!sites[i].language && ++unqualified>1)goto done;
        if(sites[i].neutral_root){chosen=(int)i;best_language=4;best_count=1;}
    }
    if(best_language==4){ok=pr_copy(url,cap,sites[chosen].url);goto done;}
    for(unsigned i=0;i<count;++i) {
        int priority=sites[i].language==3?3:!sites[i].language?2:sites[i].language==2?1:0;
        if(priority>best_language){chosen=(int)i;best_language=priority;best_count=1;}else if(priority==best_language)++best_count;
    }
    if(best_language>0 && best_count==1)ok=pr_copy(url,cap,sites[chosen].url);
done:
    free(sites);return ok;
}
static unsigned pr_claim_people(const PrJson *json,int claims,char ids[3][16])
{
    static const char *properties[]={"P178","P123"};
    unsigned count=0,p;
    for(p=0;p<2;++p) {
        int array=pr_json_field(json,claims,properties[p]),claim,best_rank=0;unsigned inspected=0;
        if(array<0 || json->tokens[array].type!='[') continue;
        for(claim=array+1;claim<json->tokens[array].next;claim=json->tokens[claim].next){int rank=pr_wd_claim_rank(json,claim);
            if(++inspected>64)return PR_WD_PERSON_LIMIT+1;if(rank>best_rank)best_rank=rank;}
        for(claim=array+1;claim<json->tokens[array].next;claim=json->tokens[claim].next) {
            int value; char id[16]; unsigned i;
            if(!best_rank || pr_wd_claim_rank(json,claim)!=best_rank) continue;
            value=pr_claim_value(json,claim);
            if(!pr_json_string(json,pr_json_field(json,value,"id"),id,sizeof(id)) || !pr_qid(id)) continue;
            for(i=0;i<count;++i) if(!strcmp(id,ids[i])) break;
            if(i==count){if(count>=PR_WD_PERSON_LIMIT)return PR_WD_PERSON_LIMIT+1;pr_copy(ids[count++],16,id);}
        }
    }
    return count;
}
static int pr_wd_non_application(const PrJson *json,int claims)
{
    int array=pr_json_field(json,claims,"P31");
    if(array>=0 && json->tokens[array].type=='[')for(int claim=array+1;claim<json->tokens[array].next;claim=json->tokens[claim].next){
        char id[16];int value;if(!pr_current_claim(json,claim))continue;value=pr_claim_value(json,claim);
        if(pr_json_string(json,pr_json_field(json,value,"id"),id,sizeof(id)) && (!strcmp(id,"Q235557") || !strcmp(id,"Q317623")))return 1;
    }
    return 0; /* P31을 모른다는 이유로 소프트웨어라고 확정하지 않는다. */
}
static int pr_wd_get(PrNet *net,const char *url,PrResponse *response,PrJson *json)
{
    int error;
    if(pr_cancelled(&net->result->request) || net->result->request_count>=PROGRAM_RESEARCH_REQUEST_LIMIT || !PROGRAM_RESEARCH_WD_FETCH(net,url,1,response)) return 0;
    if(response->length>PROGRAM_RESEARCH_BODY_LIMIT || !pr_json_parse(json,response->body,response->length)) {
        net->result->error=ERROR_INVALID_DATA;
        pr_copy(net->result->failure_text,sizeof(net->result->failure_text),"공개 자료 서비스의 응답 형식을 확인하지 못했습니다. 제품 정보 없음으로 판단하지 않습니다."); return 0;
    }
    error=pr_json_field(json,0,"error");
    if(error>=0) {
        char code[128]={0};
        pr_json_string(json,pr_json_field(json,error,"code"),code,sizeof(code));
        net->result->error=ERROR_RETRY;
        snprintf(net->result->failure_text,sizeof(net->result->failure_text),
            "공개 자료 서비스가 요청을 처리하지 못했습니다(%s). 혼잡·제한 또는 서비스 오류이며 제품 정보 없음으로 판단하지 않습니다. 자동 재시도하지 않습니다.",code[0]?code:"API 오류");
        free(json->tokens); json->tokens=NULL; return 0;
    }
    return 1;
}
typedef struct PrDiscovery { char product_url[2048],publisher_url[2048],entity_url[256]; int relation_verified; } PrDiscovery;
typedef struct PrWdCandidate {char id[16],url[2048],people[3][16],publisher[16];unsigned person_count;} PrWdCandidate;
static void pr_wd_clear(PrResponse *response,PrJson *json) {free(json->tokens);memset(json,0,sizeof(*json));pr_response_free(response);}
static int pr_wd_entity(const PrJson *json,int entities,const char *id)
{
    int entity=pr_json_field(json,entities,id);char actual[16];
    if(entity<0 || pr_json_field(json,entity,"missing")>=0 || !pr_json_string(json,pr_json_field(json,entity,"id"),actual,sizeof(actual)) || strcmp(actual,id))return -1;
    return entity;
}
static int pr_wd_property(PrNet *net,const char *id,const char *property,PrResponse *response,PrJson *json)
{
    char url[256];
    if(!pr_qid(id) || (strcmp(property,"P31") && strcmp(property,"P856") && strcmp(property,"P178") && strcmp(property,"P123")))return 0;
    snprintf(url,sizeof(url),"https://www.wikidata.org/w/api.php?action=wbgetclaims&entity=%s&property=%s&format=json",id,property);
    if(!pr_wd_get(net,url,response,json))return 0;
    {int claims=pr_json_field(json,0,"claims");if(claims<0 || json->tokens[claims].type!='{'){
        net->result->error=ERROR_INVALID_DATA;pr_copy(net->result->failure_text,sizeof(net->result->failure_text),"공개 자료의 속성 응답을 확인하지 못했습니다. 제품 정보 없음으로 판단하지 않습니다.");return 0;}}
    return 1;
}
static int pr_wd_has_current_url(const PrJson *json,int claims)
{
    int array=pr_json_field(json,claims,"P856");
    if(array>=0 && json->tokens[array].type=='[')for(int item=array+1;item<json->tokens[array].next;item=json->tokens[item].next)
        if(pr_current_claim(json,item))return 1;
    return 0;
}
/* -1은 미해결·요청 한도·서비스 실패, 0은 제외, 1은 한도 안에서 사용 가능한 후보다.
 * 속성별로 조회해 끝없이 커질 수 있는 소프트웨어 버전 주장 전체를 내려받지 않는다. */
static int pr_wd_load_candidate(PrNet *net,const char *id,PrWdCandidate *candidate)
{
    static const char *properties[]={"P178","P123"};PrResponse response={0};PrJson json={0};int claims,ok=-1;
    memset(candidate,0,sizeof(*candidate));pr_copy(candidate->id,sizeof(candidate->id),id);
    /* 최소한 게시자 조직 조회 한 번과 실제 제품 문서 GET 한 번에 쓸 요청을 남긴다. */
    if(net->result->request_count>PROGRAM_RESEARCH_REQUEST_LIMIT || PROGRAM_RESEARCH_REQUEST_LIMIT-net->result->request_count<3)goto done;
    if(!pr_wd_property(net,id,"P31",&response,&json))goto done;
    claims=pr_json_field(&json,0,"claims");if(pr_wd_non_application(&json,claims)){ok=0;goto done;}
    pr_wd_clear(&response,&json);
    if(PROGRAM_RESEARCH_REQUEST_LIMIT-net->result->request_count<5)goto done;
    if(!pr_wd_property(net,id,"P856",&response,&json))goto done;
    claims=pr_json_field(&json,0,"claims");
    if(!pr_claim_url(&json,claims,candidate->url,sizeof(candidate->url))){ok=pr_wd_has_current_url(&json,claims)?-1:0;goto done;}
    pr_wd_clear(&response,&json);
    for(unsigned p=0;p<2;++p) {
        char ids[3][16]={{0}};unsigned count;
        if(!pr_wd_property(net,id,properties[p],&response,&json))goto done;
        count=pr_claim_people(&json,pr_json_field(&json,0,"claims"),ids);
        if(count>PR_WD_PERSON_LIMIT)goto done;
        for(unsigned i=0;i<count;++i){unsigned duplicate;
            for(duplicate=0;duplicate<candidate->person_count;++duplicate)if(!strcmp(candidate->people[duplicate],ids[i]))break;
            if(duplicate<candidate->person_count)continue;if(candidate->person_count>=PR_WD_PERSON_LIMIT)goto done;
            pr_copy(candidate->people[candidate->person_count++],16,ids[i]);}
        pr_wd_clear(&response,&json);
    }
    ok=candidate->person_count?1:0;
done:
    pr_wd_clear(&response,&json);return ok;
}
/* 이름/별칭 확인→후보별 현재 주소·개발사/게시자 조회→서명 게시자 이름 대조 순서다.
 * 동등한 후보가 여러 개이거나 확인이 미완료면 실패하고 out을 비운다. 결과가 있어도
 * 실제 공식 문서의 제목·운영주체·기능 문장은 다음 단계에서 확인해야 한다. */
static int pr_discover(PrNet *net,PrDiscovery *out)
{
    char url[2048],ids[512]={0},search_ids[5][16]={{0}},people[PR_WD_PERSON_BATCH_LIMIT][16]={{0}};
    ProgramResearchPlan plan;PrWdCandidate candidates[PR_WD_CANDIDATE_LIMIT]={0};PrResponse response={0};PrJson json={0};
    unsigned search_count=0,candidate_count=0,person_count=0,best_count=0;int entities,array,qualities[5]={0},selected=-1,ok=0;
    memset(out,0,sizeof(*out));if(!program_research_plan(&net->result->request,&plan) || pr_cancelled(&net->result->request))return 0;
    if(!pr_wd_get(net,plan.wikidata_search_url,&response,&json))goto done;
    array=pr_json_field(&json,0,"search");
    if(array>=0 && json.tokens[array].type=='[')for(int item=array+1;item<json.tokens[array].next;item=json.tokens[item].next) {
        char id[16];unsigned duplicate;
        if(!pr_json_string(&json,pr_json_field(&json,item,"id"),id,sizeof(id)) || !pr_qid(id))continue;
        for(duplicate=0;duplicate<search_count;++duplicate)if(!strcmp(search_ids[duplicate],id))break;
        if(duplicate<search_count)continue;if(search_count>=PR_WD_CANDIDATE_LIMIT)goto done;pr_copy(search_ids[search_count++],16,id);
    }
    pr_wd_clear(&response,&json);if(!search_count)goto done;
    for(unsigned i=0;i<search_count;++i){if(i)strcat_s(ids,sizeof(ids),"%7C");strcat_s(ids,sizeof(ids),search_ids[i]);}
    snprintf(url,sizeof(url),"https://www.wikidata.org/w/api.php?action=wbgetentities&ids=%s&props=labels%%7Caliases&languages=en%%7Cko%%7Cmul&format=json",ids);
    if(!pr_wd_get(net,url,&response,&json))goto done;entities=pr_json_field(&json,0,"entities");
    for(unsigned i=0;i<search_count;++i) {
        int entity=pr_wd_entity(&json,entities,search_ids[i]);if(entity>=0)qualities[i]=pr_wd_name_quality(&json,entity,net->result->request.product,0);
    }
    pr_wd_clear(&response,&json);
    for(int grade=4;grade>=3;--grade) {
        candidate_count=person_count=best_count=0;selected=-1;
        for(unsigned i=0;i<search_count;++i)if(qualities[i]==grade) {
            int loaded=pr_wd_load_candidate(net,search_ids[i],&candidates[candidate_count]);
            if(loaded<0)goto done;if(!loaded)continue;
            for(unsigned p=0;p<candidates[candidate_count].person_count;++p){unsigned duplicate;
                for(duplicate=0;duplicate<person_count;++duplicate)if(!strcmp(people[duplicate],candidates[candidate_count].people[p]))break;
                if(duplicate<person_count)continue;if(person_count>=PR_WD_PERSON_BATCH_LIMIT)goto done;
                pr_copy(people[person_count++],16,candidates[candidate_count].people[p]);}
            ++candidate_count;
        }
        if(!candidate_count)continue;
        if(net->result->request_count>=PROGRAM_RESEARCH_REQUEST_LIMIT-1)goto done;
        ids[0]=0;for(unsigned i=0;i<person_count;++i){if(i)strcat_s(ids,sizeof(ids),"%7C");strcat_s(ids,sizeof(ids),people[i]);}
        snprintf(url,sizeof(url),"https://www.wikidata.org/w/api.php?action=wbgetentities&ids=%s&props=labels%%7Caliases&languages=en%%7Cko%%7Cmul&format=json",ids);
        if(!pr_wd_get(net,url,&response,&json))goto done;entities=pr_json_field(&json,0,"entities");
        for(unsigned i=0;i<candidate_count;++i) {
            PrWdCandidate *candidate=&candidates[i];int best=0;unsigned matches=0;
            for(unsigned p=0;p<candidate->person_count;++p) {
                int entity=pr_wd_entity(&json,entities,candidate->people[p]);int quality=entity>=0?pr_wd_name_quality(&json,entity,net->result->request.publisher,1):0;
                if(quality>best){best=quality;matches=1;pr_copy(candidate->publisher,sizeof(candidate->publisher),candidate->people[p]);}else if(quality && quality==best)++matches;
            }
            if(matches>1)goto done;if(matches==1){selected=(int)i;++best_count;}
        }
        pr_wd_clear(&response,&json);
        if(best_count>1)goto done;if(!best_count)continue;
        pr_copy(out->product_url,sizeof(out->product_url),candidates[selected].url);
        snprintf(out->entity_url,sizeof(out->entity_url),"https://www.wikidata.org/wiki/%s",candidates[selected].id);
        out->relation_verified=1;
        /* 게시자 사이트는 선택 정보다. 같은 이름 후보들을 조사해 API 한도를 썼더라도
         * 실제 제품 문서 GET에 사용할 요청은 남겨 둔다. */
        if(net->result->request_count<PROGRAM_RESEARCH_REQUEST_LIMIT-1 && pr_wd_property(net,candidates[selected].publisher,"P856",&response,&json))
            (void)pr_claim_url(&json,pr_json_field(&json,0,"claims"),out->publisher_url,sizeof(out->publisher_url));
        ok=!pr_cancelled(&net->result->request);goto done;
    }
done:
    pr_wd_clear(&response,&json);if(!ok)memset(out,0,sizeof(*out));return ok;
}
#endif
