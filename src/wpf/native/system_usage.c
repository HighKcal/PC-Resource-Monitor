/* 화면 전용 시스템 CPU·GPU·물리 RAM 측정이다. 수집 worker가 PDH query 하나를 소유한다.
 * sys_read의 새 구간 → read_cpu/read_gpu → 값 반환 순서로 읽는다. 이 값은 monitor DB로
 * 보내지 않으며 query·PID 배열은 sys_close가 해제한다. 프로세스 전용 RAM은 bridge.c 경로다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

typedef struct GpuPid { DWORD pid; double percent; } GpuPid;
typedef struct SystemView {
    uint64_t utc, gpu_start, gpu_end, total_bytes, available_bytes;
    double cpu, gpu; /* 음수는 결측이다. 유효한 0과 구분한다. */
    const GpuPid *pids;
    DWORD pid_count, gpu_status, luid_high, luid_low, physical, engine;
    WCHAR engine_type[64];
} SystemView;

typedef struct Usage {
    PDH_HQUERY query;
    PDH_HCOUNTER gpu, cpu_groups;
    DWORD gpu_open_status;
    int cpu_valid;
    uint64_t kernel, user, idle, previous_ms, previous_gpu_start;
    GpuPid *pids;
} Usage;

typedef struct Instance {
    DWORD high, low, physical, engine, pid;
    const WCHAR *type;
    double value;
} Instance;

static uint64_t ticks(FILETIME ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }
static uint64_t now_utc(void) { FILETIME ft; GetSystemTimeAsFileTime(&ft); return ticks(ft); }

__declspec(dllexport) Usage *sys_open(void)
{
    Usage *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->gpu_open_status = PdhOpenQueryW(NULL, 0, &u->query);
    if (u->gpu_open_status == ERROR_SUCCESS) {
        u->gpu_open_status = PdhAddEnglishCounterW(u->query,
            L"\\GPU Engine(*)\\Utilization Percentage", 0, &u->gpu);
        if (GetActiveProcessorGroupCount() > 1)
            (void)PdhAddEnglishCounterW(u->query, L"\\Processor Information(_Total)\\% Processor Time", 0, &u->cpu_groups);
    }
    return u;
}

static void read_cpu(Usage *u, SystemView *v, int consecutive)
{
    FILETIME idle, kernel, user;
    uint64_t k, a, i;
    if (GetActiveProcessorGroupCount() > 1) {
        PDH_FMT_COUNTERVALUE value;
        if (consecutive && u->cpu_groups &&
            !PdhGetFormattedCounterValue(u->cpu_groups, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, NULL, &value) &&
            value.CStatus <= PDH_CSTATUS_NEW_DATA && value.doubleValue >= 0 && value.doubleValue <= 100)
            v->cpu = value.doubleValue;
        return;
    }
    if (!GetSystemTimes(&idle, &kernel, &user)) { u->cpu_valid = 0; return; }
    k = ticks(kernel); a = ticks(user); i = ticks(idle);
    if (consecutive && u->cpu_valid && k >= u->kernel && i >= u->idle && a >= u->user) {
        double capacity = (double)(k-u->kernel) + (double)(a-u->user);
        double rest = (double)(i-u->idle);
        if (capacity > 0 && rest <= capacity) v->cpu = 100.0 * (capacity-rest) / capacity;
    }
    u->kernel = k; u->user = a; u->idle = i; u->cpu_valid = 1;
}

static int engine_order(const Instance *a, const Instance *b)
{
    if (a->high != b->high) return a->high < b->high ? -1 : 1;
    if (a->low != b->low) return a->low < b->low ? -1 : 1;
    if (a->physical != b->physical) return a->physical < b->physical ? -1 : 1;
    return (a->engine > b->engine) - (a->engine < b->engine);
}

static int instance_order(const void *left, const void *right)
{
    const Instance *a = left, *b = right;
    int order = engine_order(a,b);
    return order ? order : (a->pid > b->pid) - (a->pid < b->pid);
}

/* PID별 비율은 한 엔진의 경과 시간 기준이다. 가장 바쁜 물리 엔진 하나만 선택한다.
 * 결측이나 중복 instance가 있으면 선택이 치우칠 수 있으므로 이번 전체 GPU를
 * 결측으로 남긴다. 임시 instance는 여기서 해제하고 PID 결과는 Usage가 소유한다. */
static DWORD busiest(Usage *u, SystemView *v, PDH_FMT_COUNTERVALUE_ITEM_W *items, DWORD n)
{
    if (!n) return PDH_NO_DATA;
    Instance *instances = calloc(n, sizeof(*instances));
    if (!instances) return ERROR_OUTOFMEMORY;
    DWORD status = PDH_INVALID_DATA, first = 0, length = 0;
    double maximum = -1;
    for (DWORD j = 0; j < n; ++j) {
        Instance *p = instances+j;
        PDH_FMT_COUNTERVALUE val = items[j].FmtValue;
        const WCHAR *s = items[j].szName;
        if (!s || val.CStatus > PDH_CSTATUS_NEW_DATA || !isfinite(val.doubleValue) || val.doubleValue < 0 ||
            swscanf_s(s,L"pid_%lu_luid_0x%lx_0x%lx_phys_%lu_eng_%lu",
                &p->pid,&p->high,&p->low,&p->physical,&p->engine) != 5) goto done;
        p->type = wcsstr(s,L"_engtype_");
        if (!p->type) goto done;
        p->type += 9;
        p->value = val.doubleValue;
    }
    qsort(instances,n,sizeof(*instances),instance_order);
    for (DWORD start = 0; start < n;) {
        DWORD end = start;
        double sum = 0;
        while (end < n && !engine_order(instances+start,instances+end)) {
            if (end > start && instances[end-1].pid == instances[end].pid) goto done;
            sum += instances[end++].value;
        }
        if (sum > 100) goto done;
        if (sum > maximum) { maximum = sum; first = start; length = end-start; }
        start = end;
    }
    u->pids = calloc(length,sizeof(*u->pids));
    if (!u->pids) { status = ERROR_OUTOFMEMORY; goto done; }
    for (DWORD j = 0; j < length; ++j) {
        u->pids[j].pid = instances[first+j].pid;
        u->pids[j].percent = instances[first+j].value;
    }
    v->gpu = maximum; v->pid_count = length; v->pids = u->pids;
    v->luid_high = instances[first].high; v->luid_low = instances[first].low;
    v->physical = instances[first].physical; v->engine = instances[first].engine;
    wcsncpy_s(v->engine_type,_countof(v->engine_type),instances[first].type,_TRUNCATE);
    status = ERROR_SUCCESS;
done:
    free(instances);
    return status;
}

static DWORD read_gpu(Usage *u,SystemView *v)
{
    DWORD size = 0, count = 0, code;
    const DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
    PDH_FMT_COUNTERVALUE_ITEM_W *buffer;
    code = PdhGetFormattedCounterArrayW(u->gpu,format,&size,&count,NULL);
    if (code != (DWORD)PDH_MORE_DATA || size == 0) return code ? code : PDH_NO_DATA;
    buffer = malloc(size);
    if (!buffer) return ERROR_OUTOFMEMORY;
    code = PdhGetFormattedCounterArrayW(u->gpu,format,&size,&count,buffer);
    if (!code) code = busiest(u,v,buffer,count);
    free(buffer);
    return code;
}

/* 이전 PID 배열을 해제하고 이번 결과를 채운다. 첫 회차·15초 초과 공백의 CPU/GPU는
 * 변화량 대신 결측을 남기고 RAM은 독립적으로 읽는다. 반환 pids는 Usage 소유이며
 * 다음 sys_read/sys_close 전 SystemUsage.Read가 복사한다. 구간 시각은 UTC 100ns다. */
__declspec(dllexport) void sys_read(Usage *u, SystemView *v)
{
    MEMORYSTATUSEX ram = {0};
    uint64_t ms = GetTickCount64(), start = now_utc();
    int consecutive = u->previous_ms && ms > u->previous_ms && ms-u->previous_ms <= 15000;
    DWORD status = u->query ? PdhCollectQueryData(u->query) : u->gpu_open_status;
    free(u->pids); u->pids = NULL;
    memset(v,0,sizeof(*v));
    v->cpu = v->gpu = -1;
    v->gpu_end = now_utc();
    v->gpu_start = u->previous_gpu_start;
    v->gpu_status = u->gpu_open_status ? u->gpu_open_status : status;
    if (!v->gpu_status) {
        v->gpu_status = consecutive && v->gpu_start && v->gpu_end > v->gpu_start ?
            read_gpu(u,v) : (DWORD)PDH_NO_DATA;
    }
    u->previous_gpu_start = status ? 0 : start;
    read_cpu(u,v,consecutive && (!u->cpu_groups || !status));
    ram.dwLength = sizeof(ram);
    if (GlobalMemoryStatusEx(&ram) && ram.ullTotalPhys && ram.ullAvailPhys <= ram.ullTotalPhys) {
        v->total_bytes = ram.ullTotalPhys;
        v->available_bytes = ram.ullAvailPhys;
    }
    v->utc = now_utc(); u->previous_ms = ms;
}

__declspec(dllexport) void sys_close(Usage *u)
{
    if (u) { if (u->query) PdhCloseQuery(u->query); free(u->pids); free(u); }
}
