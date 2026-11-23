#ifndef RESOURCE_COLLECTOR_H
#define RESOURCE_COLLECTOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Windows 데스크톱 C17 수집 API. 대상 API 버전은 _WIN32_WINNT=0x0601이다.
 * 실행 검증 환경은 Windows 11 x64이며, 모든 호환 OS를 검증한 것은 아니다.
 * UTC 값의 단위는 1601-01-01 UTC부터의 100ns다. */
typedef struct ResourceCollector ResourceCollector;

typedef enum ResourceStatus {
    RESOURCE_OK = 0,
    RESOURCE_INVALID_ARGUMENT,
    RESOURCE_OUT_OF_MEMORY,
    RESOURCE_PLATFORM_ERROR
} ResourceStatus;

typedef enum ResourceForeground {
    RESOURCE_FOREGROUND_UNKNOWN = -1,
    RESOURCE_BACKGROUND = 0,
    RESOURCE_FOREGROUND = 1
} ResourceForeground;

typedef enum ResourceDeltaStatus {
    RESOURCE_DELTA_VALID = 0,
    RESOURCE_DELTA_FIRST_SAMPLE,
    RESOURCE_DELTA_IDENTITY_UNAVAILABLE,
    RESOURCE_DELTA_IDENTITY_CHANGED,
    RESOURCE_DELTA_QUERY_FAILED,
    RESOURCE_DELTA_CLOCK_INVALID,
    RESOURCE_DELTA_LONG_GAP,
    RESOURCE_DELTA_COUNTER_ROLLBACK
} ResourceDeltaStatus;

/* 필드별 유효성은 독립적이다. 비트가 없으면 값이 0이어도 유효한 측정으로 해석하지 않는다. */
enum ResourceValidity {
    RESOURCE_VALID_NAME = 1u << 0,
    RESOURCE_VALID_IDENTITY = 1u << 1,
    RESOURCE_VALID_CPU = 1u << 2,
    RESOURCE_VALID_MEMORY = 1u << 3,
    RESOURCE_VALID_IO_READ = 1u << 4,
    RESOURCE_VALID_IO_WRITE = 1u << 5,
    RESOURCE_VALID_ELAPSED = 1u << 6,
    RESOURCE_VALID_FOREGROUND = 1u << 7,
    RESOURCE_VALID_OBSERVATION_INTERVAL = 1u << 8
};

typedef struct ResourceData {
    uint64_t measured_at_utc_100ns;
    uint32_t pid;
    const char *process_name_utf8; /* 카운터와 같은 핸들에서 얻은 실행 파일명. 배치가 소유하는
                                  * 읽기 전용 UTF-8이며, 얻지 못하면 NULL이다. */
    uint64_t creation_time_utc_100ns; /* VALID_IDENTITY가 있을 때만 유효하다. */
    uint32_t valid_fields;
    double cpu_percent;             /* 전체 논리 CPU 용량을 100%로 한 비율. */
    uint64_t working_set_bytes;     /* VALID_MEMORY가 있을 때만 유효한 WorkingSetSize 바이트. */
    /* 프로세스 누적 읽기·쓰기 바이트의 차분을 각 실제 QPC 간격으로 나눈 B/s. */
    double io_read_bytes_per_second;
    double io_write_bytes_per_second;
    double elapsed_seconds;         /* 현재 UTC와 프로세스 생성 UTC의 차이(초). */
    double observation_interval_seconds; /* 신원 확인에 성공한 두 관측 사이의 QPC 간격(초). */
    double cpu_interval_seconds;    /* CPU 카운터 관측 사이의 QPC 간격(초). */
    double io_interval_seconds;     /* I/O 카운터 관측 사이의 QPC 간격(초). */
    ResourceForeground foreground; /* 회차에서 한 번 관측한 전경 여부이며 지속 시간은 아니다. */
    ResourceDeltaStatus cpu_status;
    ResourceDeltaStatus io_read_status;
    ResourceDeltaStatus io_write_status;
    uint32_t open_error;            /* Win32 오류 코드. API 호출 성공 시 0. */
    uint32_t times_error;
    uint32_t memory_error;
    uint32_t io_error;
    uint32_t name_error;
} ResourceData;

typedef struct ResourceBatch {
    ResourceData *items;            /* 소비자는 읽기만 한다. */
    size_t count;
    uint64_t started_at_utc_100ns;
    uint64_t finished_at_utc_100ns;
    uint64_t foreground_observed_at_utc_100ns;
    uint32_t foreground_pid;        /* 0이면 전경 프로세스를 알 수 없다. */
    uint32_t logical_processor_count;
} ResourceBatch;

typedef struct ResourceCollectorConfig {
    double max_interval_seconds; /* 양의 유한값. 기본 15.0초이며 초과 간격은 무효다. */
} ResourceCollectorConfig;

typedef struct ResourceCollectorInfo {
    size_t tracked_processes;       /* 현재 회차의 상태 수. 프로세스 핸들은 보관하지 않는다. */
    uint32_t logical_processor_count;
    uint64_t successful_batches;
} ResourceCollectorInfo;

/* config가 NULL이면 기본 설정을 사용한다. 실패 시 *out은 NULL이다. */
ResourceStatus resource_collector_create(const ResourceCollectorConfig *config,
    ResourceCollector **out, uint32_t *win32_error);

/* 한 회차만 수집하며 대기하지 않는다. *out은 0 초기화되었거나 이미 해제되어야 한다.
 * 플랫폼·할당 실패 시 출력과 이전 카운터 상태를 비운다. 잘못된 인자는 기존 출력·상태를
 * 바꾸지 않고 거부한다. 개별 프로세스의 API 실패는 회차 전체의 실패로 처리하지 않고
 * 각 필드의 유효성·오류에 남긴다. 같은 collector를 동시 또는 재귀 호출하지 않는다. */
ResourceStatus resource_collector_collect(ResourceCollector *collector,
    ResourceBatch *out, uint32_t *win32_error);

/* 이 배치의 배열·이름만 해제하고 *batch를 비운다. 반복 해제해도 안전하다.
 * 배치는 다음 수집이나 collector 해제 뒤에도 유효하다. */
void resource_batch_free(ResourceBatch *batch);
void resource_collector_destroy(ResourceCollector *collector);
void resource_collector_get_info(const ResourceCollector *collector,
    ResourceCollectorInfo *out);
const char *resource_delta_status_string(ResourceDeltaStatus status);

#ifdef __cplusplus
}
#endif
#endif
