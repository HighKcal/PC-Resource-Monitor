# Collector 계약

공개 인터페이스는 [resource_collector.h](../../include/resource_collector.h), 구현은 `collector/resource_collector.c`, 공유 계산·상태 전이는 `collector/resource_core.c`다. 내부 `resource_core.h`는 제품과 계산 테스트의 연결용이다.

2026-11-03 선택적 표시 API는 [resource_display.h](../../include/resource_display.h)에 추가했다. 기존 구조체 ABI와 `RESOURCE_VALID_NAME` 의미는 유지한다. `resource_collector_collect_display`는 같은 한 회차에서 `ResourceBatch`와 별도 소유 `ResourceDisplayBatch`를 반환한다. 목록 이름은 핸들 이름을 못 얻었을 때만 참고 표시하며 원본 유효 비트·신원·경로·이력으로 승격하지 않는다. 표시 진단의 검증 범위는 [검증 요약](../validation.md)을 따른다.

## 호출과 소유권

`resource_collector_create` → `resource_collector_collect` → `resource_batch_free` → `resource_collector_destroy` 순서로 사용한다. 공개 collect는 한 회차만 수행하며 대기는 호출자가 담당한다. `ResourceBatch batch = {0}`으로 시작한다.

배열과 UTF-8 문자열은 batch 소유이며 읽기 전용으로 취급한다. 다음 수집이나 collector 파괴 후에도 해제 전까지 유효하다. `resource_batch_free`로 한 번에 해제하며 빈 batch의 반복 해제는 안전하다. 소유 중인 batch를 다시 넘기면 인자를 거부하고 기존 결과를 보존한다. 내부 이전 상태와 결과 메모리는 분리되어 있다. 동일 collector의 동시·재귀 호출은 지원하지 않는다.

표시 batch도 독립 소유이며 `resource_display_batch_free`로 해제한다. 양 배열은 같은 호출의 index로 결합하고 count·PID·측정 UTC·유효 생성시각을 대조한다. 다른 회차나 신원불명 행을 PID만으로 연결하지 않는다. HANDLE은 기존 raw 이름, LIST는 sidecar가 소유한 `szExeFile` 관측 이름, UNAVAILABLE은 이름 미확보다. 표시 메모리 할당 실패도 전체 수집 실패로 처리해 양 출력과 이전 기준을 비운다. 기존 collect는 표시 메타데이터를 만들지 않는 호환 wrapper다.

## 단위와 유효성

| 값 | 계약 |
|---|---|
| CPU | `(kernel + user 증가 100ns / 실제 QPC 초 / 전체 논리 CPU 수) × 100`. PC 전체 용량 대비 백분율. |
| RAM | `WorkingSetSize` bytes. 공유 상주 페이지도 포함하며 PrivateUsage를 뜻하지 않는다. |
| I/O | 프로세스 Read/WriteTransferCount 증가 / 실제 해당 QPC 구간의 초. B/s이며 물리 디스크 전용 계측은 아니다. |
| UTC | 1601 epoch 이후 100ns FILETIME 단위의 기록 시각. 100ns 측정 정밀도를 보장하지 않는다. |
| 관측 간격 | 행의 QPC 간격과 CPU/I/O API별 QPC 간격을 별도로 전달. 설정된 대기 시간을 분모로 사용하지 않는다. |
| 실행 인스턴스 | 유효한 PID + 생성 UTC 시각. 이름이나 PID 단독으로 이어 붙이지 않는다. |
| 이름 | 같은 열린 핸들의 실행 이미지 basename, UTF-8. 전체 경로는 별도 monitor identity 어댑터가 재검증해 복사한다. |
| foreground | 회차 시작 foreground 창의 소유 PID와 비교한 순간 관측. `1`, `0`, unknown `-1`; 실제 사용 시간이나 필요성을 뜻하지 않는다. |

소비자는 모든 필드의 `valid_fields`를 먼저 확인한다. 부동소수 결측은 NAN도 사용하지만, 정수 0 또는 오류 코드 0으로 유효성을 대체하면 안 된다. 유효한 0은 실제 0이다. 저장에서는 무효 필드를 NULL로 변환한다.

첫 표본, 실패, 신원 불확실, PID 재사용, 카운터 역행, QPC 이상, 기본 15초 초과 공백은 해당 변화량을 무효화한다. 필드별 실패는 해당 필드의 이전 기준을 끊으며 다른 정상 필드는 유지한다. identity가 불확실하면 변화량 기준을 모두 끊는다.

## 권한과 실패

일반 사용자 권한에서 `PROCESS_QUERY_LIMITED_INFORMATION`으로 조회한다. 접근 거부·조회 중 종료는 해당 행/필드의 유효 비트와 Win32 오류로 전달하며 권한 상승을 시도하지 않는다. `open_error` 뒤의 조회는 미호출일 수 있으므로 오류 0이 성공 증거가 아니다.

선택적 표시 진단은 실제 API 시도 bit를 제공한다. 열린 핸들의 개별 조회가 실패했을 때만 `GetExitCodeProcess`로 후속 종료를 확인한다. 종료 관측은 그 API 실패의 원인을 확정하는 값이 아니며 다른 필드의 첫 표본 상태를 덮지 않는다. `STILL_ACTIVE`는 생존 확정으로 쓰지 않는다. PID 0/4는 운영체제 특수 항목으로 표시하고, 오류 5만으로 보호 프로세스를 판정하지 않는다. 권한·반환값 해석은 아래 공식 API 문서와 실제 필드별 유효성을 함께 확인한다.

스냅샷 열거·메모리 할당 등 전체 실패는 부분 결과와 내부 이전 기준을 비우며 다음 성공 회차는 첫 표본으로 처리한다. 잘못된 인자의 거부는 기존 상태를 보존한다. 핸들은 회차 내 닫는다. 목록에서 사라진 인스턴스의 상태는 제거한다.

## 공식 근거와 구현 정책의 구분

- [GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes): 생성시각, kernel/user 누계 100ns, 필요한 조회 권한과 실패 시 GetLastError.
- [PROCESS_MEMORY_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters), [GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo): WorkingSetSize 단위와 권한·반환값.
- [GetProcessIoCounters](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessiocounters), [IO_COUNTERS](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-io_counters): 프로세스 누적 I/O와 byte 계수.
- [QPC 지침](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps): QPC는 외부 UTC 기준과 독립적인 구간 측정 시간원.
- [QueryFullProcessImageNameW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-queryfullprocessimagenamew): 같은 핸들의 전체 이미지 경로, 권한과 실패 조건.

2026-10-26 원문을 확인했다. CPU 정규화 공식, 15초 기준, 결측·상태 연결 규칙은 위 API를 이용해 이 프로젝트가 정한 계약이다. 짧은 수집 사이에 생성·종료한 프로세스, 실제 사용자 의도, 프로세스의 악성·불필요 여부는 관측으로 확정할 수 없다.
