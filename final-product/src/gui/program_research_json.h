/* 공개 서비스 JSON을 제한된 토큰 배열로 읽는 내부 파서다. pr_json_parse→field/string
 * 순서로 사용한다. 원문은 빌리고 tokens만 할당하므로 해석이 끝날 때까지 원문을 유지한 뒤
 * 호출자가 free(json.tokens)한다. 실패는 빈 성공 응답이 아니라 0/-1로 전달한다. */
#ifndef PROGRAM_RESEARCH_JSON_H
#define PROGRAM_RESEARCH_JSON_H
/* start/end는 원문 바이트 경계, next는 하위 값을 모두 건너뛴 다음 토큰 인덱스다.
 * count는 생성한 토큰 수이며 capacity와 원문 length(바이트)를 서로 바꾸어 쓰지 않는다. */
typedef struct PrJsonToken { int type,start,end,next; } PrJsonToken;
typedef struct PrJson { const char *text; size_t length,pos; PrJsonToken *tokens; unsigned count,capacity; } PrJson;
static void pr_json_space(PrJson *json) { while(json->pos<json->length && strchr(" \t\r\n",json->text[json->pos])) ++json->pos; }
static int pr_json_value(PrJson *json,unsigned depth)
{
    unsigned index;
    char c,close=0;
    if(depth>32 || json->pos>=json->length || json->count>=json->capacity) return -1;
    index=json->count++; c=json->text[json->pos++];
    json->tokens[index].start=(int)json->pos-1;
    json->tokens[index].type=c;
    if(c=='{' || c=='[') {
        close=c=='{'?'}':']'; pr_json_space(json);
        if(json->pos<json->length && json->text[json->pos]==close) ++json->pos;
        else for(;;) {
            if(c=='{' && (json->pos>=json->length || json->text[json->pos]!='"')) return -1;
            if(pr_json_value(json,depth+1)<0) return -1;
            pr_json_space(json);
            if(c=='{') {
                if(json->pos>=json->length || json->text[json->pos++]!=':') return -1;
                pr_json_space(json); if(pr_json_value(json,depth+1)<0) return -1; pr_json_space(json);
            }
            if(json->pos>=json->length) return -1;
            if(json->text[json->pos]==close) { ++json->pos; break; }
            if(json->text[json->pos++]!=',') return -1; pr_json_space(json);
        }
    } else if(c=='"') {
        int escaped=0,ended=0;
        while(json->pos<json->length) {
            unsigned char ch=(unsigned char)json->text[json->pos++];
            if(ch<32) return -1;
            if(!escaped && ch=='"') { ended=1; break; }
            if(!escaped && ch=='\\') escaped=1; else escaped=0;
        }
        if(!ended) return -1;
    } else {
        while(json->pos<json->length && !strchr(" \t\r\n,}]",json->text[json->pos])) ++json->pos;
        {
            const char *value=json->text+json->tokens[index].start;
            size_t n=json->pos-(size_t)json->tokens[index].start,p=0;
            if(!((n==4 && !memcmp(value,"true",4)) || (n==5 && !memcmp(value,"false",5)) || (n==4 && !memcmp(value,"null",4)))) {
                if(p<n && value[p]=='-') ++p;
                if(p>=n || !isdigit((unsigned char)value[p])) return -1;
                if(value[p]=='0') ++p; else while(p<n && isdigit((unsigned char)value[p])) ++p;
                if(p<n && value[p]=='.') { ++p; if(p>=n || !isdigit((unsigned char)value[p])) return -1; while(p<n && isdigit((unsigned char)value[p])) ++p; }
                if(p<n && (value[p]=='e' || value[p]=='E')) { ++p; if(p<n && (value[p]=='+' || value[p]=='-')) ++p; if(p>=n || !isdigit((unsigned char)value[p])) return -1; while(p<n && isdigit((unsigned char)value[p])) ++p; }
                if(p!=n) return -1;
            }
        }
    }
    json->tokens[index].end=(int)json->pos; json->tokens[index].next=(int)json->count;
    return (int)index;
}
static int pr_json_parse(PrJson *json,const char *text,size_t length)
{
    memset(json,0,sizeof(*json)); json->text=text; json->length=length; json->capacity=32768;
    if(!length || length>INT_MAX || !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,(int)length,NULL,0)) return 0;
    json->tokens=(PrJsonToken *)calloc(json->capacity,sizeof(*json->tokens)); if(!json->tokens) return 0;
    pr_json_space(json);
    if(pr_json_value(json,0)!=0) { free(json->tokens); json->tokens=NULL; return 0; }
    pr_json_space(json); if(json->pos!=length) { free(json->tokens); json->tokens=NULL; return 0; }
    return 1;
}
static int pr_json_equal(const PrJson *json,int index,const char *value)
{
    const PrJsonToken *token;
    if(index<0 || (unsigned)index>=json->count) return 0; token=&json->tokens[index];
    return token->type=='"' && (size_t)(token->end-token->start-2)==strlen(value) &&
        !memcmp(json->text+token->start+1,value,strlen(value));
}
/* 같은 키가 두 번 나오면 어느 값을 택할지 추정하지 않고 -1을 돌려준다.
 * 따라서 호출부는 키 부재와 모호한 중복 모두 '사용 가능한 근거 없음'으로 다뤄야 한다. */
static int pr_json_field(const PrJson *json,int object,const char *key)
{
    int i,found=-1;
    if(object<0 || json->tokens[object].type!='{') return -1;
    for(i=object+1;i<json->tokens[object].next;) {
        int value=i+1;
        if(pr_json_equal(json,i,key)) { if(found>=0) return -1; found=value; }
        i=json->tokens[value].next;
    }
    return found;
}
static int pr_json_literal(const PrJson *json,int index,const char *value)
{
    return index>=0 && (unsigned)index<json->count && json->tokens[index].type!='"' &&
        (size_t)(json->tokens[index].end-json->tokens[index].start)==strlen(value) &&
        !memcmp(json->text+json->tokens[index].start,value,strlen(value));
}
static int pr_utf8_scalar(unsigned value,char *out,size_t cap,size_t *used)
{
    unsigned count=value<0x80?1:value<0x800?2:value<0x10000?3:4;
    if(value>0x10ffff || (value>=0xd800 && value<=0xdfff) || *used+count>=cap) return 0;
    if(count==1) out[(*used)++]=(char)value;
    else {
        unsigned i;
        out[*used]=(char)((count==2?0xc0:count==3?0xe0:0xf0)|(value>>(6*(count-1))));
        for(i=1;i<count;++i) out[*used+i]=(char)(0x80|((value>>(6*(count-1-i)))&0x3f));
        *used+=count;
    }
    return 1;
}
static int pr_hex(char c) { return c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:-1; }
/* JSON 문자열을 호출자 버퍼에 풀어 쓰되 NUL 공간을 남긴다. 유니코드 surrogate 쌍과
 * 용량을 확인하며 실패 때 부분 출력이 있을 수 있으므로 반환 1일 때만 out을 사용한다. */
static int pr_json_string(const PrJson *json,int index,char *out,size_t cap)
{
    int i,end; size_t used=0;
    if(!cap) return 0; out[0]=0;
    if(index<0 || (unsigned)index>=json->count || json->tokens[index].type!='"') return 0;
    i=json->tokens[index].start+1; end=json->tokens[index].end-1;
    while(i<end) {
        unsigned char c=(unsigned char)json->text[i++];
        if(c=='\\') {
            char escape; if(i>=end) return 0; escape=json->text[i++];
            if(escape=='u') {
                unsigned value=0; int j;
                if(i+4>end) return 0;
                for(j=0;j<4;++j) { int hex=pr_hex(json->text[i++]); if(hex<0) return 0; value=(value<<4)|(unsigned)hex; }
                if(value>=0xd800 && value<=0xdbff) {
                    unsigned low=0;
                    if(i+6>end || json->text[i++]!='\\' || json->text[i++]!='u') return 0;
                    for(j=0;j<4;++j) { int hex=pr_hex(json->text[i++]); if(hex<0) return 0; low=(low<<4)|(unsigned)hex; }
                    if(low<0xdc00 || low>0xdfff) return 0; value=0x10000+((value-0xd800)<<10)+(low-0xdc00);
                }
                if(value<32 || !pr_utf8_scalar(value,out,cap,&used)) return 0;
            } else {
                if(escape!='"' && escape!='\\' && escape!='/') return 0;
                if(used+1>=cap) return 0; out[used++]=escape;
            }
        } else { if(used+1>=cap) return 0; out[used++]=(char)c; }
    }
    out[used]=0; return 1;
}
#endif
