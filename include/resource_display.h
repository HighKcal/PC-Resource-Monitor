#ifndef RESOURCE_DISPLAY_H
#define RESOURCE_DISPLAY_H

#include "resource_collector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 선택적 표시·진단 메타데이터. 인스턴스 신원이나 저장·그룹화·개인화 비교의 입력으로
 * 사용하지 않는다. ResourceData와 유효성 플래그의 의미는 그대로 유지한다. */
typedef enum ResourceNameSource {
    RESOURCE_NAME_UNAVAILABLE = 0,
    RESOURCE_NAME_HANDLE,
    RESOURCE_NAME_LIST
} ResourceNameSource;

typedef enum ResourceSpecialProcess {
    RESOURCE_SPECIAL_NONE = 0,
    RESOURCE_SPECIAL_IDLE,   /* PID 0: 운영체제 유휴 항목. */
    RESOURCE_SPECIAL_SYSTEM  /* PID 4: 운영체제 System 항목. */
} ResourceSpecialProcess;

enum ResourceQueryAttempt {
    RESOURCE_QUERY_TIMES = 1u << 0,
    RESOURCE_QUERY_MEMORY = 1u << 1,
    RESOURCE_QUERY_IO = 1u << 2,
    RESOURCE_QUERY_NAME = 1u << 3,
    RESOURCE_QUERY_EXIT = 1u << 4
};

typedef struct ResourceDisplayInfo {
    uint32_t pid;
    uint64_t creation_time_utc_100ns; /* 원시 행에 VALID_IDENTITY가 없으면 0. */
    uint64_t measured_at_utc_100ns;   /* 대응하는 원시 행의 측정 시각과 같다. */
    ResourceNameSource name_source;
    const char *snapshot_name_utf8; /* 표시 배치가 소유하며 LIST일 때만 이름을 담는다.
                                    * 목록 관측값이며 핸들로 확인한 신원은 아니다. */
    uint32_t snapshot_name_error;
    uint32_t attempted_fields;      /* OpenProcess 실패 시 0. */
    uint32_t exit_query_error;
    uint32_t exit_code;             /* QUERY_EXIT 시도·성공 후에만 유효하다. */
    int exit_observed;              /* 필드 실패 뒤 조회한 종료 코드가 STILL_ACTIVE가 아니면 참.
                                    * 거짓이라고 생존이 확인된 것은 아니다. */
    ResourceSpecialProcess special_process;
} ResourceDisplayInfo;

typedef struct ResourceDisplayBatch {
    ResourceDisplayInfo *items;
    size_t count;
} ResourceDisplayBatch;

/* collect와 같은 한 회차에서 동일한 Toolhelp 목록의 표시 배열을 함께 만든다.
 * 인덱스로 대응시키고 사용 전 PID·생성시각·측정시각을 확인한다. 다음 배치와 PID만으로
 * 연결하지 않는다. HANDLE은 ResourceData.process_name_utf8을 사용한다. LIST는 표시
 * 전용이며 RESOURCE_VALID_NAME이나 VALID_IDENTITY를 설정하지 않는다.
 * display가 NULL이면 메타데이터를 생략한다. 두 출력은 비어 있어야 한다. 할당·플랫폼
 * 실패 시 두 출력을 해제하고 수집기 이력을 초기화한다. 잘못된 인자는 기존 출력·상태를 보존한다. */
ResourceStatus resource_collector_collect_display(ResourceCollector *collector,
    ResourceBatch *out, ResourceDisplayBatch *display, uint32_t *win32_error);

/* 다음 수집, 원시 배치 해제, 수집기 해제와 독립적으로 소유한다.
 * 문자열·배열을 해제하고 배치를 비운다. 반복 해제해도 안전하다. */
void resource_display_batch_free(ResourceDisplayBatch *display);

#ifdef __cplusplus
}
#endif
#endif
