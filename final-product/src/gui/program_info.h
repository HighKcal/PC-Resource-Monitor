/* 선택한 한 인스턴스의 읽기 전용 로컬 설명 근거다. program_info_collect는 전용
 * worker에서 호출하며 WPF 진입은 wpf/native/companion.c::pc_program_collect다.
 * 수집/DB/Baseline과 독립적이다. 지원 제품의 식별·역할·목적별
 * 설명은 program_info_roles, 시작 등록 조회 범위는 program_info_startup에서 바꾼다.
 * 요청/결과의 문자열은 값 복사다. 결과가 소유하는 process_handle은 슬롯 이동 뒤
 * 원본을 NULL로 하며, 최종 소유자는 program_info_release로 닫는다. */
#ifndef PROGRAM_INFO_H
#define PROGRAM_INFO_H
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <winver.h>

#pragma comment(lib,"wintrust.lib")
#pragma comment(lib,"crypt32.lib")
#pragma comment(lib,"version.lib")
#pragma comment(lib,"advapi32.lib")
#pragma warning(push)
#pragma warning(disable:4505)
#include "native_program_info_origin.h"

#define PROGRAM_INFO_PATH_CAP 32768
#define PROGRAM_INFO_RESULT_LIFETIME_MS 60000u
#define PROGRAM_INFO_RUN_CHARS 4096u
#define PROGRAM_INFO_VERSION_LIMIT (1024u * 1024u)

typedef enum ProgramInfoIntent {
    PROGRAM_INFO_INTENT_UNSET=0, PROGRAM_INFO_INTENT_AUTO, PROGRAM_INFO_INTENT_MANUAL
} ProgramInfoIntent;
typedef enum ProgramInfoIdentity {
    PROGRAM_INFO_ID_UNVERIFIED=0, PROGRAM_INFO_ID_VERIFIED, PROGRAM_INFO_ID_STALE,
    PROGRAM_INFO_ID_SYNTHETIC, PROGRAM_INFO_ID_CANCELLED
} ProgramInfoIdentity;
typedef enum ProgramInfoRole {
    PROGRAM_INFO_ROLE_NONE=0, PROGRAM_INFO_ROLE_STEAM, PROGRAM_INFO_ROLE_ONEDRIVE
} ProgramInfoRole;
typedef enum ProgramInfoStartup {
    PROGRAM_INFO_STARTUP_NOT_CHECKED=0, PROGRAM_INFO_STARTUP_MATCH,
    PROGRAM_INFO_STARTUP_NOT_FOUND, PROGRAM_INFO_STARTUP_UNKNOWN
} ProgramInfoStartup;
typedef struct ProgramInfoRequest {
    uint64_t request_id;
    unsigned session;
    DWORD pid;
    uint64_t creation;
    int synthetic;
    ProgramInfoIntent intent;
    HANDLE cancel_event; /* 빌린 stop event. worker가 끝날 때까지 호출 측이 보존한다. */
    uint64_t deadline_tick; /* GetTickCount64 단위. 0이면 시작부터 15초. */
    char path[PROGRAM_INFO_PATH_CAP];
} ProgramInfoRequest;
typedef struct ProgramInfoSource {
    char title[128], url[256], applies[384];
} ProgramInfoSource;
typedef struct ProgramInfoFileStamp {
    DWORD volume, index_high, index_low;
    uint64_t size, modified, created;
    BYTE sha256[32];
    int hash_valid;
} ProgramInfoFileStamp;
typedef struct ProgramInfoDisplayMetadata {
    int available;
    char product[256],description[512],company[256],original[256],version[128];
} ProgramInfoDisplayMetadata;
typedef struct ProgramInfoResult {
    ProgramInfoRequest request;
    HANDLE process_handle;
    uint64_t checked_utc, checked_tick, expires_tick;
    ProgramInfoIdentity identity;
    ProgramInfoRole role;
    ProgramInfoStartup startup;
    LONG signature_code;
    DWORD error;
    int metadata_verified, metadata_source_verified, signature_verified;
    DWORD metadata_error,mui_type;
    int homepage_source,homepage_conflict,opus_conflict;
    unsigned run_keys_checked, run_keys_missing, run_values_checked, run_matches;
    unsigned run_match_scopes; /* bit0/1 HKCU64/32, bit2/3 HKLM64/32. 중복 view 독립 등록 아님. */
    int run_incomplete;
    ProgramInfoFileStamp file_stamp;
    char product[256], publisher[256], company[256], version[128], original[256];
    char description[512],opus_name[256],homepage_origin[256];
    char version_url[512],opus_more_url[512],opus_publisher_url[512];
    ProgramInfoDisplayMetadata display_metadata; /* MUI 가능 표기: 역할·공개 payload에는 사용하지 않는다. */
    char identity_text[1024], role_text[1536], startup_text[1536];
    char limits_text[1536], next_text[1024];
    ProgramInfoSource sources[4];
    unsigned source_count;
} ProgramInfoResult;

/* 새로운 지원 제품은 이 레코드에 식별 조건·문장·출처를 함께 추가한다.
 * 파일명만 맞추는 fallback이나 현재 작업/사용자 목적 추정은 두지 않는다. */
typedef struct ProgramInfoRoleRecord {
    ProgramInfoRole role;
    const char *basename,*product,*company,*original,*publishers[2];
    const wchar_t *run_value;
    const char *role_text,*purpose[3],*next_text;
    unsigned source_count;
    ProgramInfoSource sources[4];
} ProgramInfoRoleRecord;
static const ProgramInfoRoleRecord program_info_roles[]={
    {
        PROGRAM_INFO_ROLE_STEAM,"steam.exe","Steam","Valve Corporation","steam.exe",{"Valve Corp.","Valve Corporation"},L"Steam",
        "Steam · 게임 실행·구매·커뮤니티와 게임 자동 업데이트를 제공하는 Valve 클라이언트입니다. 지금 어떤 작업 중인지는 확인하지 않았습니다.",
        {
            "켜두면 로그인 뒤 업데이트를 준비할 기회가 있고, 끄면 직접 연 뒤로 준비가 늦어질 수 있습니다. 자동 준비를 원하는지, 필요할 때 직접 열지 선택해 보세요.",
            "로그인 뒤 자동 준비를 원한다면 Steam 자동 시작을 켜둘 이유가 있습니다. 업데이트 시점은 다운로드 설정 등에 따르며 즉시 완료를 보장하지 않습니다.",
            "필요할 때 직접 열고 사전 준비가 필요 없다면 로그인 자동 시작 해제가 목적에 맞습니다. 직접 열기 전 업데이트 기회가 늦어질 수 있습니다. 앱 삭제·수동 실행 차단이나 자동 업데이트 기능 자체를 끄는 설정은 아닙니다."
        },
        "Steam 시작 옵션과 Windows 설정 > 앱 > 시작 프로그램에서 활성 상태를 확인하세요. 업데이트 시점이 중요하면 Steam 다운로드 설정도 확인하세요.",
        4,{
            {"Steam 공식 소개","https://store.steampowered.com/about/","Valve 오프라인 서명·제품 Steam·원래 파일명 steam.exe 일치 시 연결하는 일반 제품 역할"},
            {"Steam 시작 설정","https://help.steampowered.com/en/faqs/view/6089-6768-F7FB-C46F","로그인 자동 시작 설정의 범위. 실제 활성 상태를 조회한 결과는 아님"},
            {"Steam 다운로드·업데이트","https://help.steampowered.com/en/faqs/view/71ab-698d-57eb-178c","업데이트 시점은 클라이언트·게임별 다운로드 설정 등 조건에 따름. 즉시 완료 보장 없음"},
            {"Windows 시작 앱 설정","https://support.microsoft.com/en-us/windows/experience/startup-boot/configure-startup-applications-in-windows","로그인 자동 시작 설정. Run의 Steam 값만 조회하며 다른 이름·서비스·예약 작업·시작 폴더·패키지 시작은 미조회"}
        }
    },
    {
        PROGRAM_INFO_ROLE_ONEDRIVE,"OneDrive.exe","Microsoft OneDrive","Microsoft Corporation","OneDrive.exe",{"Microsoft Corporation",NULL},L"OneDrive",
        "Microsoft OneDrive · PC와 클라우드 사이의 파일 동기화를 제공하는 클라이언트입니다. 현재 계정 연결·동기화 진행·오류는 확인하지 않았습니다.",
        {
            "켜두면 로그인 뒤 동기화를 준비할 수 있고, 끄면 직접 열기 전 PC의 새 변경 내용이 동기화되지 않을 수 있습니다. 자동 준비와 필요할 때 직접 열기 중 원하는 방식을 선택해 보세요.",
            "로그인 뒤 파일 동기화를 자동 준비하려면 OneDrive 자동 시작을 켜둘 이유가 있습니다. 계정 연결·네트워크·동기화 설정이 필요하며 동기화 완료를 보장하지 않습니다.",
            "필요할 때 직접 열어 동기화하려면 로그인 자동 시작 해제를 검토할 수 있습니다. 직접 열기 전 새 변경 내용이 동기화되지 않을 수 있습니다. 앱 삭제나 클라우드 파일 삭제를 뜻하지 않으며 수동으로 다시 열 수 있습니다."
        },
        "Windows 설정 > 앱 > 시작 프로그램의 활성 상태와 OneDrive 아이콘의 동기화 상태를 확인하세요. 계정 연결·네트워크·정책에 따라 동작이 달라집니다.",
        3,{
            {"OneDrive 파일 동기화","https://support.microsoft.com/en-us/onedrive/sync-your-computer-s-files-and-folders-with-onedrive","Microsoft 오프라인 서명·제품 Microsoft OneDrive·원래 파일명 OneDrive.exe 일치 시 연결. 계정 연결·온라인 등 조건 필요"},
            {"OneDrive 일시 중지·종료","https://support.microsoft.com/en-us/onedrive/how-to-cancel-or-stop-sync-in-onedrive","앱 종료와 다시 열기의 일반 동작. 현재 동기화 완료·오류나 사용자 파일 상태를 확인한 결과는 아님"},
            {"Windows 시작 앱 설정","https://support.microsoft.com/en-us/windows/experience/startup-boot/configure-startup-applications-in-windows","로그인 자동 시작 설정. Run의 OneDrive 값만 조회하며 다른 이름·서비스·예약 작업·시작 폴더·패키지 시작은 미조회"},
            {"","",""}
        }
    }
};
static const ProgramInfoRoleRecord *program_info_role_record(ProgramInfoRole role)
{
    size_t i;
    for(i=0;i<sizeof(program_info_roles)/sizeof(program_info_roles[0]);++i)
        if(program_info_roles[i].role==role) return &program_info_roles[i];
    return NULL;
}

static uint64_t program_info_ft(FILETIME value)
{
    return ((uint64_t)value.dwHighDateTime << 32) | value.dwLowDateTime;
}
static int program_info_wequal(const wchar_t *a,const wchar_t *b)
{
    return CompareStringOrdinal(a,-1,b,-1,TRUE)==CSTR_EQUAL;
}
static int program_info_utf8(const wchar_t *value,char *out,size_t cap)
{
    size_t i;
    if(!out || !cap) return 0;
    out[0]=0;
    if(!value || cap>INT_MAX || !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,
        value,-1,out,(int)cap,NULL,NULL)) return 0;
    /* 파일 메타데이터는 실행 지시가 아닌 한 줄 데이터로만 표시한다. */
    for(i=0;out[i];++i) if((unsigned char)out[i]<32 || out[i]==127) out[i]=' ';
    return 1;
}
static int program_info_cancelled(const ProgramInfoRequest *request)
{
    return (request->cancel_event && WaitForSingleObject(request->cancel_event,0)!=WAIT_TIMEOUT) ||
        (request->deadline_tick && GetTickCount64()>=request->deadline_tick);
}
static void program_info_release(ProgramInfoResult *result)
{
    if(result && result->process_handle) {
        CloseHandle(result->process_handle); result->process_handle=NULL;
    }
}
/* UI는 보관한 프로세스 핸들의 생존과 GetTickCount64 만료만 빠르게 확인한다.
 * 파일을 다시 읽지 않으며 신원·파일 재대조가 필요한 호출자는 program_info_recheck를 쓴다. */
static int program_info_result_current(const ProgramInfoResult *result)
{
    return result && result->identity==PROGRAM_INFO_ID_VERIFIED && result->process_handle &&
        GetTickCount64()<result->expires_tick &&
        WaitForSingleObject(result->process_handle,0)==WAIT_TIMEOUT;
}
static int program_info_stamp(HANDLE file,ProgramInfoFileStamp *stamp)
{
    BY_HANDLE_FILE_INFORMATION info;
    memset(stamp,0,sizeof(*stamp));
    if(!GetFileInformationByHandle(file,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) return 0;
    stamp->volume=info.dwVolumeSerialNumber;
    stamp->index_high=info.nFileIndexHigh; stamp->index_low=info.nFileIndexLow;
    stamp->size=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;
    stamp->modified=program_info_ft(info.ftLastWriteTime);
    stamp->created=program_info_ft(info.ftCreationTime);
    return 1;
}
static int program_info_hash(HANDLE file,const ProgramInfoRequest *request,BYTE digest[32])
{
    HCRYPTPROV provider=0; HCRYPTHASH hash=0;
    BYTE buffer[65536]; DWORD read=0,bytes=32;
    LARGE_INTEGER zero={0},saved;
    uint64_t total=0;
    int ok=0;
    if(!SetFilePointerEx(file,zero,&saved,FILE_CURRENT) || !SetFilePointerEx(file,zero,NULL,FILE_BEGIN)) return 0;
    if(!CryptAcquireContextW(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)) goto done;
    for(;;) {
        if(program_info_cancelled(request) || !ReadFile(file,buffer,sizeof(buffer),&read,NULL)) goto done;
        if(!read) break;
        total+=read;
        if(total>UINT64_C(536870912) || !CryptHashData(hash,buffer,read,0)) goto done;
    }
    ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&bytes,0) && bytes==32;
done:
    if(hash) CryptDestroyHash(hash); if(provider) CryptReleaseContext(provider,0);
    (void)SetFilePointerEx(file,saved,NULL,FILE_BEGIN); return ok;
}
static int program_info_stamp_equal(const ProgramInfoFileStamp *a,const ProgramInfoFileStamp *b)
{
    return a->volume==b->volume && a->index_high==b->index_high && a->index_low==b->index_low &&
        a->size==b->size && a->modified==b->modified && a->created==b->created;
}
static int program_info_process_matches(HANDLE process,const ProgramInfoRequest *request,const wchar_t *path)
{
    FILETIME created,exited,kernel,user;
    DWORD count=PROGRAM_INFO_PATH_CAP;
    wchar_t *current=(wchar_t *)malloc(PROGRAM_INFO_PATH_CAP*sizeof(wchar_t));
    int matches=0;
    if(!current) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    if(WaitForSingleObject(process,0)==WAIT_TIMEOUT &&
        GetProcessTimes(process,&created,&exited,&kernel,&user) &&
        program_info_ft(created)==request->creation &&
        QueryFullProcessImageNameW(process,0,current,&count) && !wcscmp(current,path)) matches=1;
    else if(GetLastError()==ERROR_SUCCESS) SetLastError(ERROR_INVALID_DATA);
    free(current); return matches;
}
/* UNC/장치 경로 및 원격 드라이브는 조회하지 않는다. 재분석점도 허용하지 않는다.
 * 각 상위 경로를 먼저 열고 delete 공유를 막아 다음 경로를 여는 동안 교체를 막는다. */
typedef struct ProgramInfoLocalFile {
    HANDLE file,parents[256];
    unsigned count;
} ProgramInfoLocalFile;
static void program_info_close_local(ProgramInfoLocalFile *opened)
{
    unsigned i;
    if(opened->file!=INVALID_HANDLE_VALUE) CloseHandle(opened->file);
    for(i=0;i<opened->count;++i) CloseHandle(opened->parents[i]);
    opened->file=INVALID_HANDLE_VALUE; opened->count=0;
}
static HANDLE program_info_open_local(const wchar_t *path,ProgramInfoLocalFile *opened,DWORD *error)
{
    wchar_t *prefix;
    size_t length=wcslen(path),position;
    wchar_t root[4]={0};
    DWORD drive;
    memset(opened,0,sizeof(*opened)); opened->file=INVALID_HANDLE_VALUE;
    if(length<4 || length>=PROGRAM_INFO_PATH_CAP || path[1]!=L':' || path[2]!=L'\\' ||
        !((path[0]>=L'A' && path[0]<=L'Z') || (path[0]>=L'a' && path[0]<=L'z'))) {
        *error=ERROR_NOT_SUPPORTED; return INVALID_HANDLE_VALUE;
    }
    root[0]=path[0]; root[1]=L':'; root[2]=L'\\'; drive=GetDriveTypeW(root);
    if(drive!=DRIVE_FIXED && drive!=DRIVE_RAMDISK && drive!=DRIVE_REMOVABLE) {
        *error=ERROR_NOT_SUPPORTED; return INVALID_HANDLE_VALUE;
    }
    prefix=(wchar_t *)malloc((length+1)*sizeof(*prefix));
    if(!prefix) { *error=ERROR_NOT_ENOUGH_MEMORY; return INVALID_HANDLE_VALUE; }
    memcpy(prefix,path,(length+1)*sizeof(*prefix));
    for(position=3;position<=length;++position) {
        wchar_t saved;
        BY_HANDLE_FILE_INFORMATION info;
        HANDLE part;
        if(prefix[position]!=L'\\' && prefix[position]!=0) continue;
        if(position==3 || prefix[position-1]==L'\\' ||
            (prefix[position-1]==L'.' && (position==4 || prefix[position-2]==L'\\' ||
            (prefix[position-2]==L'.' && (position==5 || prefix[position-3]==L'\\'))))) {
            *error=ERROR_NOT_SUPPORTED; goto done;
        }
        if(opened->count>=256) { *error=ERROR_BUFFER_OVERFLOW; goto done; }
        saved=prefix[position]; prefix[position]=0;
        part=CreateFileW(prefix,position==length?GENERIC_READ:FILE_READ_ATTRIBUTES,
            position==length?FILE_SHARE_READ:(FILE_SHARE_READ|FILE_SHARE_WRITE),NULL,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,NULL);
        prefix[position]=saved;
        if(part==INVALID_HANDLE_VALUE) { *error=GetLastError(); goto done; }
        if(!GetFileInformationByHandle(part,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            (position<length && !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))) {
            *error=ERROR_NOT_SUPPORTED; CloseHandle(part); goto done;
        }
        if(position==length) { opened->file=part; break; }
        opened->parents[opened->count++]=part;
    }
done:
    if(opened->file==INVALID_HANDLE_VALUE) program_info_close_local(opened);
    free(prefix); return opened->file;
}
static int program_info_version_string(void *data,WORD language,WORD codepage,
    const wchar_t *name,char *out,size_t cap)
{
    wchar_t query[128],*text=NULL;
    UINT length=0;
    swprintf_s(query,sizeof(query)/sizeof(*query),L"\\StringFileInfo\\%04x%04x\\%s",language,codepage,name);
    if(!VerQueryValueW(data,query,(void **)&text,&length) || !text || !length || length>4096 ||
        text[length-1]!=0) return 0;
    return program_info_utf8(text,out,cap);
}
/* VersionInfoW는 LN 파일의 문자열을 별도 MUI에서 병합할 수 있다. 선택 파일만으로
 * 역할을 연결하기 위해 자체 리소스(non-LN)가 확인된 파일만 허용한다. 다른 유형·실패는 보류한다. */
static int program_info_metadata_source(const wchar_t *path,DWORD *type,DWORD *error)
{
    FILEMUIINFO info;
    DWORD bytes=sizeof(info);
    memset(&info,0,sizeof(info)); info.dwSize=sizeof(info); info.dwVersion=1;
    if(!GetFileMUIInfo(MUI_QUERY_TYPE|MUI_QUERY_RESOURCE_TYPES,path,&info,&bytes)) {
        *error=GetLastError(); return 0;
    }
    *type=info.dwFileType;
    if(info.dwFileType!=MUI_FILETYPE_NOT_LANGUAGE_NEUTRAL) { *error=ERROR_NOT_SUPPORTED; return 0; }
    return 1;
}
static void program_info_metadata(const wchar_t *path,ProgramInfoResult *result)
{
    DWORD ignored=0,size;
    void *data;
    struct ProgramInfoTranslation { WORD language,codepage; } *translations=NULL;
    UINT bytes=0;
    result->metadata_verified=result->metadata_source_verified=0;
    result->metadata_error=result->mui_type=0;
    result->product[0]=result->company[0]=result->original[0]=result->version[0]=result->description[0]=result->version_url[0]=0;
    result->metadata_source_verified=program_info_metadata_source(path,&result->mui_type,&result->metadata_error);
    if(!result->metadata_source_verified) return;
    size=GetFileVersionInfoSizeW(path,&ignored);
    if(!size || size>PROGRAM_INFO_VERSION_LIMIT) { result->metadata_error=size?ERROR_BUFFER_OVERFLOW:GetLastError(); return; }
    data=malloc(size); if(!data) return;
    if(GetFileVersionInfoW(path,0,size,data) &&
        VerQueryValueW(data,L"\\VarFileInfo\\Translation",(void **)&translations,&bytes) &&
        bytes>=sizeof(*translations)) {
        WORD language=translations[0].language,codepage=translations[0].codepage;
        int product=program_info_version_string(data,language,codepage,L"ProductName",result->product,sizeof(result->product));
        int company=program_info_version_string(data,language,codepage,L"CompanyName",result->company,sizeof(result->company));
        int original=program_info_version_string(data,language,codepage,L"OriginalFilename",result->original,sizeof(result->original));
        (void)program_info_version_string(data,language,codepage,L"FileVersion",result->version,sizeof(result->version));
        (void)program_info_version_string(data,language,codepage,L"FileDescription",result->description,sizeof(result->description));
        if(!program_info_version_string(data,language,codepage,L"URL",result->version_url,sizeof(result->version_url)) &&
            !program_info_version_string(data,language,codepage,L"CompanyURL",result->version_url,sizeof(result->version_url)))
            (void)program_info_version_string(data,language,codepage,L"HomePage",result->version_url,sizeof(result->version_url));
        result->metadata_verified=product && company && original;
    }
    free(data);
}
static void program_info_opus(CMSG_SIGNER_INFO *signer,ProgramInfoResult *result)
{
    unsigned opus_count=0;
    if(!signer || signer->AuthAttrs.cAttr>64 || !signer->AuthAttrs.rgAttr) return;
    for(DWORD i=0;i<signer->AuthAttrs.cAttr;++i) if(signer->AuthAttrs.rgAttr[i].pszObjId &&
        !strcmp(signer->AuthAttrs.rgAttr[i].pszObjId,SPC_SP_OPUS_INFO_OBJID)) ++opus_count;
    if(opus_count>1) { result->opus_conflict=1; return; }
    for(DWORD i=0;i<signer->AuthAttrs.cAttr;++i) {
        CRYPT_ATTRIBUTE *attribute=&signer->AuthAttrs.rgAttr[i];
        SPC_SP_OPUS_INFO *opus=NULL;
        DWORD size=0;
        if(!attribute->pszObjId || strcmp(attribute->pszObjId,SPC_SP_OPUS_INFO_OBJID) ||
            attribute->cValue!=1 || !attribute->rgValue || !attribute->rgValue[0].pbData ||
            !attribute->rgValue[0].cbData || attribute->rgValue[0].cbData>65536) continue;
        if(!CryptDecodeObjectEx(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,SPC_SP_OPUS_INFO_OBJID,
            attribute->rgValue[0].pbData,attribute->rgValue[0].cbData,CRYPT_DECODE_ALLOC_FLAG,NULL,&opus,&size)) continue;
        if(opus && size<=65536) {
            if(opus->pwszProgramName && wcsnlen(opus->pwszProgramName,256)<256)
                (void)program_info_utf8(opus->pwszProgramName,result->opus_name,sizeof(result->opus_name));
            if(opus->pMoreInfo && opus->pMoreInfo->dwLinkChoice==SPC_URL_LINK_CHOICE && opus->pMoreInfo->pwszUrl &&
                wcsnlen(opus->pMoreInfo->pwszUrl,512)<512)
                (void)program_info_utf8(opus->pMoreInfo->pwszUrl,result->opus_more_url,sizeof(result->opus_more_url));
            if(opus->pPublisherInfo && opus->pPublisherInfo->dwLinkChoice==SPC_URL_LINK_CHOICE && opus->pPublisherInfo->pwszUrl &&
                wcsnlen(opus->pPublisherInfo->pwszUrl,512)<512)
                (void)program_info_utf8(opus->pPublisherInfo->pwszUrl,result->opus_publisher_url,sizeof(result->opus_publisher_url));
        }
        LocalFree(opus);
        break;
    }
}
static void program_info_display_metadata(const wchar_t *path,ProgramInfoResult *result)
{
    DWORD ignored=0,size;
    void *data;
    struct DisplayTranslation { WORD language,codepage; } *translations=NULL;
    UINT bytes=0;
    memset(&result->display_metadata,0,sizeof(result->display_metadata));
    if(result->metadata_source_verified) return;
    size=GetFileVersionInfoSizeW(path,&ignored);
    if(!size || size>PROGRAM_INFO_VERSION_LIMIT) return;
    data=malloc(size); if(!data) return;
    if(GetFileVersionInfoW(path,0,size,data) && VerQueryValueW(data,L"\\VarFileInfo\\Translation",(void **)&translations,&bytes) && bytes>=sizeof(*translations)) {
        WORD language=translations[0].language,codepage=translations[0].codepage;
        ProgramInfoDisplayMetadata *display=&result->display_metadata;
        display->available=program_info_version_string(data,language,codepage,L"ProductName",display->product,sizeof(display->product));
        display->available|=program_info_version_string(data,language,codepage,L"FileDescription",display->description,sizeof(display->description));
        (void)program_info_version_string(data,language,codepage,L"CompanyName",display->company,sizeof(display->company));
        (void)program_info_version_string(data,language,codepage,L"OriginalFilename",display->original,sizeof(display->original));
        (void)program_info_version_string(data,language,codepage,L"FileVersion",display->version,sizeof(display->version));
    }
    free(data);
}
static void program_info_homepage(ProgramInfoResult *result)
{
    const char *urls[]={result->opus_more_url,result->opus_publisher_url,result->metadata_source_verified?result->version_url:""};
    char origin[256];
    result->homepage_origin[0]=0; result->homepage_source=result->homepage_conflict=0;
    if(result->opus_conflict) { result->homepage_conflict=1; return; }
    if(!result->signature_verified || result->signature_code!=ERROR_SUCCESS) return;
    for(unsigned i=0;i<3;++i) if(program_info_https_origin(urls[i],origin,sizeof(origin))) {
        if(result->homepage_source && strcmp(result->homepage_origin,origin)) {
            result->homepage_conflict=1; result->homepage_origin[0]=0; result->homepage_source=0; return;
        }
        if(!result->homepage_source) {
            snprintf(result->homepage_origin,sizeof(result->homepage_origin),"%s",origin);
            result->homepage_source=(int)i+1;
        }
    }
}
static void program_info_signature(const wchar_t *path,HANDLE file,ProgramInfoResult *result)
{
    GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO info;
    WINTRUST_DATA trust;
    memset(&info,0,sizeof(info)); memset(&trust,0,sizeof(trust));
    info.cbStruct=sizeof(info); info.pcwszFilePath=path; info.hFile=file;
    trust.cbStruct=sizeof(trust); trust.dwUIChoice=WTD_UI_NONE;
    trust.fdwRevocationChecks=WTD_REVOKE_NONE; trust.dwUnionChoice=WTD_CHOICE_FILE;
    trust.pFile=&info; trust.dwStateAction=WTD_STATEACTION_VERIFY;
    /* Microsoft 문서: REVOKE_NONE만으로는 network 금지가 아니다. CACHE_ONLY가 필수다. */
    trust.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_NONE;
    result->signature_code=WinVerifyTrust(INVALID_HANDLE_VALUE,&policy,&trust);
    if(result->signature_code==ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA *provider=WTHelperProvDataFromStateData(trust.hWVTStateData);
        CRYPT_PROVIDER_SGNR *signer=provider?WTHelperGetProvSignerFromChain(provider,0,FALSE,0):NULL;
        if(signer && !signer->dwError && signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
            wchar_t publisher[256];
            DWORD length=CertGetNameStringW(signer->pasCertChain[0].pCert,CERT_NAME_SIMPLE_DISPLAY_TYPE,0,NULL,
                publisher,(DWORD)(sizeof(publisher)/sizeof(*publisher)));
            if(length>1 && length<sizeof(publisher)/sizeof(*publisher) &&
                program_info_utf8(publisher,result->publisher,sizeof(result->publisher))) {
                result->signature_verified=1;
                program_info_opus(signer->psSigner,result);
            }
        }
    }
    trust.dwStateAction=WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(INVALID_HANDLE_VALUE,&policy,&trust);
    program_info_homepage(result);
}
/* 등록은 실행 경로의 첫 직접 토큰만 비교한다. 간접 명령·애매한 비인용 경로는 unknown.
 * 명령줄/레지스트리 값/인수는 실행하지 않고 Result에도 복사하지 않는다. */
static int program_info_run_target(const wchar_t *command,wchar_t *out,size_t cap)
{
    const wchar_t *start=command,*end;
    size_t length;
    while(*start==L' ' || *start==L'\t') ++start;
    if(*start==L'"') {
        ++start; end=wcschr(start,L'"');
        if(!end || (end[1] && end[1]!=L' ' && end[1]!=L'\t')) return 0;
    } else {
        end=start; while(*end && *end!=L' ' && *end!=L'\t') ++end;
    }
    length=(size_t)(end-start);
    if(length<7 || length>=cap || start[1]!=L':' || start[2]!=L'\\' ||
        _wcsnicmp(end-4,L".exe",4)!=0) return 0;
    if(wmemchr(start,L'%',length) || wmemchr(start,L'\n',length) || wmemchr(start,L'\r',length)) return 0;
    memcpy(out,start,length*sizeof(*out)); out[length]=0; return 1;
}
static void program_info_run_key(const ProgramInfoRequest *request,HKEY root,REGSAM view,
    unsigned scope,const wchar_t *value,const wchar_t *path,ProgramInfoResult *result)
{
    HKEY key=NULL;
    wchar_t command[PROGRAM_INFO_RUN_CHARS],expanded[PROGRAM_INFO_RUN_CHARS],target[PROGRAM_INFO_RUN_CHARS];
    DWORD bytes=sizeof(command),type=0;
    LONG status;
    if(program_info_cancelled(request)) { result->run_incomplete=1; return; }
    status=RegOpenKeyExW(root,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE|view,&key);
    if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { ++result->run_keys_missing; return; }
    if(status!=ERROR_SUCCESS) { result->run_incomplete=1; return; }
    ++result->run_keys_checked;
    /* 지원 제품의 알려진 값 하나만 읽는다. 전체 앱 inventory는 열거하지 않는다. */
    status=RegQueryValueExW(key,value,NULL,&type,(BYTE *)command,&bytes);
    RegCloseKey(key);
    if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) { ++result->run_keys_missing; return; }
    ++result->run_values_checked;
    if(status!=ERROR_SUCCESS || (type!=REG_SZ && type!=REG_EXPAND_SZ) || bytes<sizeof(wchar_t) ||
        bytes>sizeof(command) || bytes%sizeof(wchar_t) || command[bytes/sizeof(wchar_t)-1]!=0 ||
        wcslen(command)!=bytes/sizeof(wchar_t)-1) { result->run_incomplete=1; return; }
    if(type==REG_EXPAND_SZ) {
        DWORD needed=ExpandEnvironmentStringsW(command,expanded,(DWORD)(sizeof(expanded)/sizeof(*expanded)));
        if(!needed || needed>sizeof(expanded)/sizeof(*expanded)) { result->run_incomplete=1; return; }
    } else memcpy(expanded,command,bytes);
    if(!program_info_run_target(expanded,target,sizeof(target)/sizeof(*target))) {
        result->run_incomplete=1; return;
    }
    if(!wcscmp(target,path)) { ++result->run_matches; result->run_match_scopes|=scope; }
    else if(program_info_wequal(target,path)) {
        /* 대소문자만 같은 문자열은 case-sensitive 디렉터리에서 다른 객체일 수 있다. */
        ProgramInfoLocalFile registered;
        ProgramInfoFileStamp stamp;
        DWORD error=ERROR_SUCCESS;
        if(program_info_open_local(target,&registered,&error)!=INVALID_HANDLE_VALUE &&
            program_info_stamp(registered.file,&stamp) && program_info_stamp_equal(&result->file_stamp,&stamp)) {
            ++result->run_matches; result->run_match_scopes|=scope;
        } else result->run_incomplete=1;
        program_info_close_local(&registered);
    }
}
/* 지원 제품의 알려진 Run 값만 조회한다. 일치/찾지 못함/조회 불완전을 나누며,
 * 일치가 있어도 활성 여부나 이번 실행의 원인을 증명하지 않는다. 레지스트리는 읽기만 한다. */
static void program_info_startup(const ProgramInfoRequest *request,const wchar_t *path,ProgramInfoResult *result)
{
    const ProgramInfoRoleRecord *role=program_info_role_record(result->role);
    const char *name;
    if(!role) {
        result->startup=PROGRAM_INFO_STARTUP_NOT_CHECKED;
        snprintf(result->startup_text,sizeof(result->startup_text),
            "시작 등록 미조사 · 지원 제품 신원이 확인된 Steam·OneDrive의 알려진 Run 값만 조회합니다.");
        return;
    }
    name=role->product;
    /* HKCU 양쪽 view는 같은 값을 보여줄 수 있다. 개수는 독립 등록 수가 아니다. */
    program_info_run_key(request,HKEY_CURRENT_USER,KEY_WOW64_64KEY,1,role->run_value,path,result);
    program_info_run_key(request,HKEY_CURRENT_USER,KEY_WOW64_32KEY,2,role->run_value,path,result);
    program_info_run_key(request,HKEY_LOCAL_MACHINE,KEY_WOW64_64KEY,4,role->run_value,path,result);
    program_info_run_key(request,HKEY_LOCAL_MACHINE,KEY_WOW64_32KEY,8,role->run_value,path,result);
    result->startup=result->run_matches?PROGRAM_INFO_STARTUP_MATCH:
        (result->run_incomplete?PROGRAM_INFO_STARTUP_UNKNOWN:PROGRAM_INFO_STARTUP_NOT_FOUND);
    if(result->startup==PROGRAM_INFO_STARTUP_MATCH)
        snprintf(result->startup_text,sizeof(result->startup_text),
            "로그인 시작 등록 확인 · %s%s%s의 알려진 %s Run 값이 선택 파일과 일치합니다. 활성 여부와 이번 실행의 원인은 미확인입니다.%s",
            result->run_match_scopes&3?"모니터 실행 계정(HKCU)":"",
            (result->run_match_scopes&3) && (result->run_match_scopes&12)?"·":"",
            result->run_match_scopes&12?"PC 전체(HKLM)":"",name,
            result->run_incomplete?" 일부 조회는 미확인입니다.":"");
    else if(result->startup==PROGRAM_INFO_STARTUP_NOT_FOUND)
        snprintf(result->startup_text,sizeof(result->startup_text),
            "모니터 실행 계정·PC 전체 Run의 알려진 %s 값에서 선택 경로 일치 등록을 찾지 못했습니다. 다른 이름·시작 방식은 미조사이며 활성 여부와 이번 실행의 원인은 미확인입니다.",name);
    else snprintf(result->startup_text,sizeof(result->startup_text),
        "시작 등록 미확인 · 알려진 %s Run 값 조회에 실패·한도·간접 명령 또는 경로 불확실성이 있어 없다고 판단하지 않습니다. 활성 여부와 이번 실행의 원인은 미확인입니다.",name);
}
static ProgramInfoRole program_info_identify_role(const ProgramInfoResult *result)
{
    const char *base=strrchr(result->request.path,'\\');
    size_t i;
    base=base?base+1:result->request.path;
    if(result->identity!=PROGRAM_INFO_ID_VERIFIED || !result->metadata_verified || !result->metadata_source_verified ||
        !result->signature_verified || result->signature_code!=ERROR_SUCCESS) return PROGRAM_INFO_ROLE_NONE;
    for(i=0;i<sizeof(program_info_roles)/sizeof(program_info_roles[0]);++i) {
        const ProgramInfoRoleRecord *role=&program_info_roles[i];
        if(!_stricmp(base,role->basename) && !_stricmp(result->product,role->product) &&
            !_stricmp(result->company,role->company) && !_stricmp(result->original,role->original) &&
            (!_stricmp(result->publisher,role->publishers[0]) ||
            (role->publishers[1] && !_stricmp(result->publisher,role->publishers[1])))) return role->role;
    }
    return PROGRAM_INFO_ROLE_NONE;
}
static void program_info_source(ProgramInfoResult *result,const char *title,const char *url,const char *applies)
{
    ProgramInfoSource *source;
    if(result->source_count>=sizeof(result->sources)/sizeof(result->sources[0])) return;
    source=&result->sources[result->source_count++];
    snprintf(source->title,sizeof(source->title),"%s",title);
    snprintf(source->url,sizeof(source->url),"%s",url);
    snprintf(source->applies,sizeof(source->applies),"%s",applies);
}
static void program_info_role_text(ProgramInfoResult *result)
{
    const ProgramInfoRoleRecord *role=program_info_role_record(result->role);
    result->source_count=0;
    if(role) {
        snprintf(result->role_text,sizeof(result->role_text),"%s",role->role_text);
        snprintf(result->next_text,sizeof(result->next_text),"%s",role->next_text);
        memcpy(result->sources,role->sources,role->source_count*sizeof(result->sources[0]));
        result->source_count=role->source_count;
    } else {
        snprintf(result->role_text,sizeof(result->role_text),
            "지원 역할 자료가 없거나 제품·발행자·원래 파일명 조건이 맞지 않아 역할 연결을 보류했습니다. 이름이나 자원 사용량으로 필요성을 정하지 않습니다.");
        snprintf(result->next_text,sizeof(result->next_text),
            "표시된 실행 경로·제품·발행자를 직접 대조하고 해당 제작사의 공식 설명을 확인하세요. 시작 상태는 Windows 설정 > 앱 > 시작 프로그램에서 확인할 수 있습니다.");
        program_info_source(result,"Windows 시작 앱 설정","https://support.microsoft.com/en-us/windows/experience/startup-boot/configure-startup-applications-in-windows",
            "로그인 자동 시작 설정의 일반 범위. 미지원 제품의 시작 등록은 미조사");
    }
}
static void program_info_purpose_text(const ProgramInfoResult *result,ProgramInfoIntent intent,char *out,size_t cap)
{
    const char *text;
    const ProgramInfoRoleRecord *role=result?program_info_role_record(result->role):NULL;
    if(!out || !cap) return;
    if(!result || result->identity!=PROGRAM_INFO_ID_VERIFIED) text=
        "선택한 인스턴스의 신원을 확인한 뒤 목적별 설명을 제공합니다. 다시 확인하세요.";
    else if(!role) text=
        "지원 역할 근거가 없어 자동 시작 유지·해제를 추천하지 않습니다. 제작사 설명과 본인의 사용 목적을 먼저 확인하세요.";
    else text=role->purpose[(unsigned)intent<=PROGRAM_INFO_INTENT_MANUAL?intent:PROGRAM_INFO_INTENT_UNSET];
    snprintf(out,cap,"%s",text);
}
static void program_info_fail(ProgramInfoResult *result,ProgramInfoIdentity identity,DWORD error,const char *reason)
{
    result->identity=identity; result->error=error; result->role=PROGRAM_INFO_ROLE_NONE;
    result->source_count=0; result->metadata_verified=result->metadata_source_verified=result->signature_verified=0;
    result->startup=PROGRAM_INFO_STARTUP_NOT_CHECKED;
    result->product[0]=result->publisher[0]=result->company[0]=result->version[0]=result->original[0]=0;
    result->description[0]=result->opus_name[0]=result->homepage_origin[0]=0;
    result->version_url[0]=result->opus_more_url[0]=result->opus_publisher_url[0]=0;
    result->homepage_source=result->homepage_conflict=result->opus_conflict=0;
    memset(&result->display_metadata,0,sizeof(result->display_metadata));
    snprintf(result->identity_text,sizeof(result->identity_text),"%s · 오류 %lu",reason,(unsigned long)error);
    snprintf(result->role_text,sizeof(result->role_text),"신원 확인이 완료되지 않아 제품 역할과 목적별 판단을 보류합니다.");
    snprintf(result->startup_text,sizeof(result->startup_text),"선택 인스턴스와의 연결 미확인 · 등록 없음으로 해석하지 않습니다.");
    snprintf(result->next_text,sizeof(result->next_text),"현재 목록에서 같은 인스턴스를 다시 선택하고 다시 확인하세요. 실패가 계속되면 실행 경로와 접근 가능한 권한을 직접 확인하세요.");
    program_info_release(result);
}
/* 빈 결과에 요청을 복사하고 인스턴스→로컬 파일→메타데이터/서명→지원 역할/Run 등록
 * →인스턴스·파일 재검증 순서로 채운다. 실패는 identity/error와 설명으로 남고 보유 핸들은
 * 닫힌다. 성공 결과의 process_handle만 호출자에게 넘기며 나머지 임시 자원은 여기서 닫는다.
 * 기존 결과를 재사용할 호출자는 먼저 program_info_release해야 한다. */
static void program_info_collect(const ProgramInfoRequest *request,ProgramInfoResult *result)
{
    wchar_t *path=NULL;
    HANDLE file=INVALID_HANDLE_VALUE;
    ProgramInfoLocalFile opened,reopened;
    ProgramInfoFileStamp after;
    FILETIME now;
    DWORD error=ERROR_SUCCESS;
    if(!result) return;
    memset(&opened,0,sizeof(opened)); opened.file=INVALID_HANDLE_VALUE;
    memset(&reopened,0,sizeof(reopened)); reopened.file=INVALID_HANDLE_VALUE;
    memset(result,0,sizeof(*result));
    result->signature_code=TRUST_E_SUBJECT_NOT_TRUSTED;
    if(!request) { program_info_fail(result,PROGRAM_INFO_ID_UNVERIFIED,ERROR_INVALID_PARAMETER,"요청 정보 없음"); return; }
    result->request=*request;
    if(!result->request.deadline_tick) result->request.deadline_tick=GetTickCount64()+15000;
    snprintf(result->limits_text,sizeof(result->limits_text),
        "조회 범위: 선택 PID+생성시각·정확한 실행 경로, 현재 디스크 파일 객체·버전과 캐시 전용 오프라인 내장 서명. 파일 자체 리소스(non-LN)를 확인할 수 없으면 메타데이터 역할 연결을 보류합니다. 지원 제품에만 HKCU/HKLM Run 32/64비트의 Steam 또는 OneDrive 값 하나씩 조회합니다(최대4096문자). HKCU는 모니터 실행 계정이며 대상 앱 계정 일치는 미확인입니다. 다른 시작 방식·활성·실행 원인은 미확인입니다. 서명은 안전·필요성 보장이 아니며 온라인 폐기 확인은 안 합니다. 디스크 버전은 로드된 이미지 버전 증명이 아닙니다. 표시 시각의 근거이며 60초 뒤 재확인해야 합니다.");
    if(request->synthetic) {
        program_info_fail(result,PROGRAM_INFO_ID_SYNTHETIC,ERROR_NOT_SUPPORTED,"합성 화면 · 실제 프로세스 조회 생략"); return;
    }
    if(!request->pid || !request->creation || !memchr(request->path,0,sizeof(request->path)) || !request->path[0]) {
        program_info_fail(result,PROGRAM_INFO_ID_UNVERIFIED,ERROR_INVALID_PARAMETER,"PID·생성시각·경로 미확인"); return;
    }
    if(program_info_cancelled(&result->request)) goto cancelled;
    path=(wchar_t *)malloc(PROGRAM_INFO_PATH_CAP*sizeof(*path));
    if(!path) { error=ERROR_NOT_ENOUGH_MEMORY; goto failed; }
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,request->path,-1,path,PROGRAM_INFO_PATH_CAP)) { error=GetLastError(); goto failed; }
    result->process_handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,request->pid);
    if(!result->process_handle) { error=GetLastError(); goto failed; }
    SetLastError(ERROR_SUCCESS);
    if(!program_info_process_matches(result->process_handle,request,path)) { error=GetLastError(); goto stale; }
    file=program_info_open_local(path,&opened,&error);
    if(file==INVALID_HANDLE_VALUE || !program_info_stamp(file,&result->file_stamp)) { if(!error) error=GetLastError(); goto failed; }
    if(program_info_cancelled(&result->request)) goto cancelled;
    program_info_metadata(path,result);
    program_info_display_metadata(path,result);
    if(program_info_cancelled(&result->request)) goto cancelled;
    program_info_signature(path,file,result);
    if(program_info_cancelled(&result->request)) goto cancelled;
    result->identity=PROGRAM_INFO_ID_VERIFIED;
    result->role=program_info_identify_role(result);
    program_info_startup(&result->request,path,result);
    if(program_info_cancelled(&result->request)) goto cancelled;
    /* 상위 디렉터리와 파일의 교체를 막은 상태로 경로를 다시 열어 객체·버전을 확인한다. */
    if(!program_info_stamp(file,&after) || !program_info_stamp_equal(&result->file_stamp,&after)) { error=ERROR_FILE_INVALID; goto stale; }
    if(program_info_open_local(path,&reopened,&error)==INVALID_HANDLE_VALUE || !program_info_stamp(reopened.file,&after) ||
        !program_info_stamp_equal(&result->file_stamp,&after)) { if(!error) error=ERROR_FILE_INVALID; goto stale; }
    SetLastError(ERROR_SUCCESS);
    if(!program_info_process_matches(result->process_handle,request,path)) { error=GetLastError(); goto stale; }
    result->identity=PROGRAM_INFO_ID_VERIFIED;
    result->role=program_info_identify_role(result);
    GetSystemTimeAsFileTime(&now); result->checked_utc=program_info_ft(now);
    result->checked_tick=GetTickCount64(); result->expires_tick=result->checked_tick+PROGRAM_INFO_RESULT_LIFETIME_MS;
    snprintf(result->identity_text,sizeof(result->identity_text),
        "선택 인스턴스 재확인 · PID+생성시각·실행 경로 일치. 디스크 파일 객체·크기·수정시각도 전후 일치합니다. 파일 메타데이터: %s. 오프라인 서명: %s (0x%08lX).",
        result->metadata_source_verified?"자체 리소스 확인":"출처 미확인·역할 보류",
        result->signature_verified?"발행자 확인":"발행자 미확인",(unsigned long)result->signature_code);
    program_info_role_text(result);
    /* 공개 자료 조회는 파일 객체/시각 외에 내용 지문도 요구한다. 제한 안에서 SHA-256을
     * 얻지 못해도 이미 만든 로컬 설명은 유지하고 외부 조회 연결만 보류한다. */
    if(result->signature_verified && result->file_stamp.size<=UINT64_C(536870912)) {
        uint64_t tick=GetTickCount64();
        if(!result->request.deadline_tick || (result->request.deadline_tick>tick && result->request.deadline_tick-tick>=1000)) {
            ProgramInfoRequest binding=result->request;
            binding.deadline_tick=tick+750;
            result->file_stamp.hash_valid=program_info_hash(file,&binding,result->file_stamp.sha256);
        }
    }
    goto done;
cancelled:
    program_info_fail(result,PROGRAM_INFO_ID_CANCELLED,ERROR_CANCELLED,"조회 취소·시간 제한"); goto done;
stale:
    program_info_fail(result,PROGRAM_INFO_ID_STALE,error?error:ERROR_INVALID_DATA,"인스턴스 종료·신원/파일 변화 또는 재검증 실패"); goto done;
failed:
    program_info_fail(result,PROGRAM_INFO_ID_UNVERIFIED,error?error:ERROR_INVALID_DATA,"로컬 근거 조회 실패");
done:
    program_info_close_local(&opened);
    program_info_close_local(&reopened);
    free(path);
}
/* worker용 재대조 helper다. 호출 시 요청의 PID+생성시각·경로와 예상 파일 객체/시각·
 * SHA-256을 로컬 상태와 대조한다. 성공은 1, 불일치·취소·조회 실패는 0이다.
 * 설명 메타데이터를 다시 수집하거나 네트워크를 호출하지 않는다. 입력은 빌린 값이며
 * 여기서 연 프로세스/파일/상위 경로 핸들은 여기서 모두 닫는다. */
static int program_info_recheck(const ProgramInfoRequest *request,const ProgramInfoFileStamp *expected)
{
    ProgramInfoLocalFile opened;
    ProgramInfoFileStamp stamp;
    wchar_t *path;
    HANDLE process;
    DWORD error=0;
    int valid=0;
    if(!request || !expected || request->synthetic || program_info_cancelled(request)) return 0;
    path=malloc(PROGRAM_INFO_PATH_CAP*sizeof(*path));
    if(!path) return 0;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,request->path,-1,path,PROGRAM_INFO_PATH_CAP)) { free(path); return 0; }
    process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,request->pid);
    if(process) {
        if(program_info_process_matches(process,request,path) &&
            program_info_open_local(path,&opened,&error)!=INVALID_HANDLE_VALUE) {
            valid=expected->hash_valid && program_info_stamp(opened.file,&stamp) && program_info_stamp_equal(&stamp,expected) &&
                program_info_hash(opened.file,request,stamp.sha256) && !memcmp(stamp.sha256,expected->sha256,sizeof(stamp.sha256)) &&
                program_info_process_matches(process,request,path);
            program_info_close_local(&opened);
        }
        CloseHandle(process);
    }
    free(path); return valid;
}

#pragma warning(pop)
#endif
