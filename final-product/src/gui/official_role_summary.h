/* 확인한 공개 원문의 구체적인 기능 구절만 한국어 고정 문장으로 옮기는 순수 함수다.
 * 기능 문장 선택에 제품/도메인별 대응표나 설치 앱 목록을 쓰지 않는다. 제품명은 원문의
 * 주어 대조에만 쓰며 원문과 근거 인덱스를 함께 남긴다. 불명확/부정 문장은 해석을 보류한다. */
#ifndef OFFICIAL_ROLE_SUMMARY_H
#define OFFICIAL_ROLE_SUMMARY_H
#include <string.h>
#include <stdio.h>
#include <ctype.h>
typedef struct OfficialRoleSummary {
    int interpreted;
    unsigned evidence_index;
    char text_ko[1024],quote[2048];
} OfficialRoleSummary;
static int official_role_phrase(const char *text,const char *phrase)
{
    size_t n=strlen(phrase);
    for(size_t i=0;text[i];++i) {
        size_t j=0;
        if(i && isalnum((unsigned char)text[i-1])) continue;
        while(j<n && text[i+j] && tolower((unsigned char)text[i+j])==tolower((unsigned char)phrase[j])) ++j;
        if(j==n && !isalnum((unsigned char)text[i+n])) return 1;
    }
    return 0;
}
static int official_role_negative(const char *text)
{
    static const char *const words[]={"not","no","never","without","cannot","can't","doesn't","don't","isn't","aren't","unlike","unsupported","except","instead of","whose","whereas","while","which","competitor","alternative","but","although","because","when","if","unless","after","since","until","provided that"};
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);++i) if(official_role_phrase(text,words[i])) return 1;
    return strchr(text,'(') || strchr(text,')') || strstr(text,"지원하지") || strstr(text,"제공하지") ||
        strstr(text,"않습니다") || strstr(text,"아닙니다") || strstr(text,"못합니다") ||
        strstr(text,"다른 도구") || strstr(text,"다른 앱") || strstr(text,"다른 프로그램");
}
/* 서술부/보어의 시작에서만 기능 구절을 찾는다. 문장 전체에서 찾으면 뒤에 나오는
 * 다른 주체의 기능을 이 제품의 기능으로 잘못 가져올 수 있다. */
static int official_role_starts(const char *text,const char *phrase,const char **after)
{
    while(*text && isspace((unsigned char)*text)) ++text;
    while(*phrase) {
        if(isspace((unsigned char)*phrase)) {
            while(*phrase && isspace((unsigned char)*phrase)) ++phrase;
            if(!isspace((unsigned char)*text)) return 0;
            while(*text && isspace((unsigned char)*text)) ++text;
        } else {
            if(!*text || tolower((unsigned char)*text)!=tolower((unsigned char)*phrase)) return 0;
            ++text; ++phrase;
        }
    }
    if(isalnum((unsigned char)*text)) return 0;
    if(after) { while(*text && isspace((unsigned char)*text)) ++text; *after=text; }
    return 1;
}
static const char *official_role_definition(const char *text)
{
    static const char *const modifiers[]={"feature-rich","digital","document","text","audio","video","development","lightweight","powerful","free","open-source"};
    static const char *const nouns[]={"tool","program","application","platform","software","spreadsheet","editor","viewer","workspace"};
    const char *after=NULL; int noun=0;
    if(official_role_starts(text,"a",&after) || official_role_starts(text,"an",&after) || official_role_starts(text,"the",&after)) text=after;
    for(unsigned count=0;count<2;++count) {
        int modifier=0;
        for(unsigned i=0;i<sizeof(modifiers)/sizeof(modifiers[0]);++i)
            if(official_role_starts(text,modifiers[i],&after)) { text=after; modifier=1; break; }
        if(!modifier) break;
    }
    for(unsigned i=0;i<sizeof(nouns)/sizeof(nouns[0]);++i)
        if(official_role_starts(text,nouns[i],&after)) { text=after; noun=1; break; }
    if(!noun) return NULL;
    /* 명사구 뒤를 제한 없이 탐색하지 않는다. 뒤에 나온 다른 도구의 편집 기능이나
     * 편집법 설명서를 이 제품 자체의 편집 기능으로 가져오지 않는다. */
    if(official_role_starts(text,"for",&after) || official_role_starts(text,"to",&after) || official_role_starts(text,"that",&after)) return after;
    return NULL;
}
static const char *official_role_action(const ProgramResearchResult *result,const char *sentence)
{
    const char *product=result->matched_product[0]?result->matched_product:result->request.product;
    const char *action=sentence,*after=NULL;
    if(product[0]) {
        if(!pr_name_prefix(sentence,product,&action)) return NULL;
    } else if(official_role_starts(action,"a",NULL) || official_role_starts(action,"an",NULL)) {
        action=official_role_definition(action);
    }
    for(unsigned depth=0;action && depth<5;++depth) {
        while(*action && isspace((unsigned char)*action)) ++action;
        if(official_role_starts(action,"can",&after) || official_role_starts(action,"to",&after)) { action=after; continue; }
        if(official_role_starts(action,"lets",&after) || official_role_starts(action,"allows",&after) ||
            official_role_starts(action,"enables",&after) || official_role_starts(action,"helps",&after)) {
            static const char *const users[]={"you","users","people","developers","the user","a user"};
            const char *own=NULL;
            for(unsigned i=0;i<sizeof(users)/sizeof(users[0]);++i)
                if(official_role_starts(after,users[i],&own)) break;
            if(!own) return NULL; /* 다른 주체가 편집하도록 한다는 문장을 이 제품의 편집 기능으로 바꾸지 않는다. */
            action=own; continue;
        }
        if(official_role_starts(action,"provides",&after)) return after;
        if(official_role_starts(action,"is",&after) || official_role_starts(action,"are",&after)) {
            if(official_role_starts(after,"designed to",&action)) continue;
            if(official_role_starts(after,"a",NULL) || official_role_starts(after,"an",NULL) ||
                official_role_starts(after,"the",NULL) || official_role_starts(after,"software",NULL)) {
                action=official_role_definition(after); continue;
            }
            return NULL;
        }
        return action;
    }
    return NULL;
}
static int official_role_subject(const ProgramResearchResult *result,const char *sentence)
{
    const char *product=result->matched_product[0]?result->matched_product:result->request.product;
    if(official_role_negative(sentence)) return 0;
    /* 이전 형식의 검증된 입력에는 이름 없는 기능 구절이 있을 수 있다. 제품명이 있는
     * 결과는 provider의 직접 주어 규칙을 다시 적용해 하위 제품/경쟁 제품의 서술부를
     * 선택한 제품의 역할로 연결하지 않는다. */
    if(!product[0]) return 1;
    if(!pr_fact(sentence)) return 0;
    if(pr_subject(sentence,product)) return 1;
    return pr_mentions(result->title,product,0) &&
        (pr_subject(sentence,"이 프로그램") || pr_subject(sentence,"이 도구"));
}
/* 입력의 공식/제품 연결 상태를 확인한 뒤 한국어 원문, 고정 영어 기능 구절 순서로 찾는다.
 * 새 구절을 지원하려면 phrases와 주어/부정 조건을 함께 본다. 해석하지 못해도 원문 근거는
 * 입력에 남고 interpreted=0과 안내만 반환한다. 파일·네트워크·프로세스 상태는 바꾸지 않는다. */
static void official_role_summarize(const ProgramResearchResult *result,OfficialRoleSummary *out)
{
    static const struct { const char *phrase,*ko; } phrases[]={
        {"create and edit documents","문서를 만들고 편집하는 기능을 제공합니다."},
        {"edit documents","문서를 편집하는 기능을 제공합니다."},
        {"sync files","파일을 동기화하는 기능을 제공합니다."},
        {"synchronize files","파일을 동기화하는 기능을 제공합니다."},
        {"sync your files","파일을 동기화하는 기능을 제공합니다."},
        {"browse the web","웹을 탐색하는 기능을 제공합니다."},
        {"send messages","메시지를 보내는 통신 기능을 제공합니다."},
        {"make video calls","영상 통화 기능을 제공합니다."},
        {"play videos","동영상을 재생하는 기능을 제공합니다."},
        {"play music","음악을 재생하는 기능을 제공합니다."},
        {"edit photos","사진을 편집하는 기능을 제공합니다."},
        {"edit images","이미지를 편집하는 기능을 제공합니다."},
        {"record live audio","오디오를 녹음하는 기능을 제공합니다."},
        {"recording and editing audio","오디오를 녹음하고 편집하는 기능을 제공합니다."},
        {"write and debug code","코드를 작성하고 디버깅하는 개발 기능을 제공합니다."},
        {"compile source code","소스 코드를 컴파일하는 개발 기능을 제공합니다."},
        {"analyze data","데이터를 분석하는 기능을 제공합니다."},
        {"manage databases","데이터베이스를 관리하는 기능을 제공합니다."}
    };
    memset(out,0,sizeof(*out));
    snprintf(out->text_ko,sizeof(out->text_ko),"공식 자료의 원문은 확인했지만 일반 역할을 한국어로 해석할 근거 구절을 찾지 못했습니다. 아래 원문과 공식 문서를 직접 확인하세요.");
    if(!result || !result->official_confirmed || !result->product_matched || !result->evidence_count ||
        (result->status!=PROGRAM_RESEARCH_COMPLETE && result->status!=PROGRAM_RESEARCH_PARTIAL)) return;
    /* 짧은 한국어 기능 문장을 먼저 찾는다. 'sync' 같은 낱말이 아니라 온전한 문장을
     * 해석과 화면에 보여 줄 인용의 단위로 사용한다. */
    for(unsigned pass=0;pass<2;++pass) for(unsigned i=0;i<result->evidence_count && i<3;++i) {
        const char *start=result->evidence[i];
        while(*start) {
            const char *end=start;
            char sentence[2048]; size_t n; unsigned hangul=0;
            while(*end && *end!='\n' && *end!=';' && *end!='!' && *end!='?' && !(*end=='.' && (end[1]==0 || end[1]==' '))) ++end;
            if(*end && *end!='\n') ++end;
            n=(size_t)(end-start);
            if(n && n<sizeof(sentence)) {
                memcpy(sentence,start,n); sentence[n]=0;
                if(official_role_subject(result,sentence)) {
                    for(size_t k=0;k+2<n;++k) if((unsigned char)sentence[k]>=0xEA && (unsigned char)sentence[k]<=0xED &&
                        ((unsigned char)sentence[k+1]&0xC0)==0x80 && ((unsigned char)sentence[k+2]&0xC0)==0x80) ++hangul;
                    if(!pass && hangul>=12 && n<900 && !strstr(sentence,"하세요") && !strstr(sentence,"하십시오") &&
                        !strstr(sentence,"http:") && !strstr(sentence,"https:") &&
                        (strstr(sentence,"문서를 편집") || strstr(sentence,"파일을 동기화") ||
                        strstr(sentence,"웹을 탐색") || strstr(sentence,"메시지를 전송") || strstr(sentence,"메시지를 보내") ||
                        strstr(sentence,"동영상을 재생") || strstr(sentence,"데이터를 분석") || strstr(sentence,"사진을 편집") || strstr(sentence,"코드를 작성") ||
                        strstr(sentence,"도구입니다") || strstr(sentence,"프로그램입니다") || strstr(sentence,"만들 수 있습니다") ||
                        strstr(sentence,"작성할 수 있습니다") || strstr(sentence,"관리할 수 있습니다") || strstr(sentence,"위한 소프트웨어"))) {
                        out->interpreted=1; out->evidence_index=i;
                        snprintf(out->text_ko,sizeof(out->text_ko),"공식 설명: %s",sentence);
                    } else if(pass) {
                        const char *action=official_role_action(result,sentence);
                        for(unsigned p=0;action && p<sizeof(phrases)/sizeof(phrases[0]);++p)
                        if(official_role_starts(action,phrases[p].phrase,NULL)) {
                            out->interpreted=1; out->evidence_index=i;
                            snprintf(out->text_ko,sizeof(out->text_ko),"%s",phrases[p].ko); break;
                        }
                    }
                    if(out->interpreted) { snprintf(out->quote,sizeof(out->quote),"%s",sentence); return; }
                }
            }
            start=end; while(*start=='\r' || *start=='\n' || *start==' ') ++start;
        }
    }
}
#endif
