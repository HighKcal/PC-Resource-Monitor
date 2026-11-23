/* 제품의 GPU 엔진 선택/프로세스 귀속 함수를 합성 PDH 항목으로 검증한다.
 * 특정 GPU/Windows 언어는 필요 없다. 같은 엔진의 합계, 유효한 0과 무효 상태를 구분하며
 * busiest가 Usage에 할당한 PID 배열은 시험이 각 성공 사례 뒤 해제한다. */
#include "../../wpf/native/system_usage.c"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(v) do { ++checks; if (!(v)) { ++failures; fprintf(stderr,"line %d: %s\n",__LINE__,#v); } } while (0)

int main(void)
{
    Usage u = {0}; SystemView v = {0};
    PDH_FMT_COUNTERVALUE_ITEM_W items[] = {
        {L"pid_83_luid_0x0_0xff_phys_0_eng_0_engtype_3D", {0}},
        {L"pid_41_luid_0x0_0xff_phys_0_eng_0_engtype_3D", {0}},
        {L"pid_83_luid_0x0_0xff_phys_0_eng_1_engtype_Copy", {0}},
        {L"pid_41_luid_0x0_0xab_phys_1_eng_0_engtype_3D", {0}}
    };
    items[0].FmtValue.doubleValue=30; items[1].FmtValue.doubleValue=40;
    items[2].FmtValue.doubleValue=60; items[3].FmtValue.doubleValue=50;
    CHECK(busiest(&u,&v,items,4)==ERROR_SUCCESS);
    CHECK(v.gpu==70 && v.pid_count==2 && v.engine==0 && v.luid_low==0xff);
    CHECK(v.pids[0].pid==41 && v.pids[0].percent==40 && v.pids[1].percent==30);
    CHECK(!wcscmp(v.engine_type,L"3D"));
    free(u.pids); u.pids=NULL;
    items[1].FmtValue.doubleValue=0;
    CHECK(busiest(&u,&v,items,2)==ERROR_SUCCESS && v.pids[0].percent==0 && v.gpu==30);
    free(u.pids); u.pids=NULL;
    items[1].FmtValue.CStatus=PDH_CSTATUS_INVALID_DATA;
    CHECK(busiest(&u,&v,items,2)!=ERROR_SUCCESS);
    items[1].FmtValue.CStatus=0; items[1].FmtValue.doubleValue=71;
    CHECK(busiest(&u,&v,items,2)!=ERROR_SUCCESS);
    items[1]=items[0]; CHECK(busiest(&u,&v,items,2)!=ERROR_SUCCESS);
    CHECK(busiest(&u,&v,items,0)!=ERROR_SUCCESS);
    printf("GPU attribution: %u checks, %u failures\n",checks,failures);
    return failures!=0;
}
