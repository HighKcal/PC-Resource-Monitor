# 구조 이동과 호환 영향

기존 로컬 코드의 기능 보존을 우선한 초기 공개 구조다. Git 저장소가 없던 폴더였고 원격도 비어 있어 기존 Git 이력을 지울 필요가 없었다. 원본 파일과 개인 측정 증거는 로컬 보존 후 공개 대상에서 제외했다.

| 기존 | 현재 |
|---|---|
| src/resource_collector.c, resource_core.c/.h | collector/의 같은 파일 |
| src/monitor_identity.c | collector/monitor_identity.c |
| src/monitor.c | engine/monitor.c + storage/monitor_storage.c + detection/baseline.c |
| src/monitor_internal.h 및 파일 내부 MonitorInstance | engine/monitor_internal.h |
| src/monitor_schema.h / docs/schema.sql | storage/monitor_schema.h / storage/schema.sql |
| examples/console.c | app/console.c |
| examples/monitor_gui.c | gui/monitor_gui.c |
| tests/test_core.c, test_windows.c | tests/collector/ |
| tests/test_monitor.c | tests/integration/ |
| tests/benchmark_monitor_*.c | tests/performance/ |
| vendor/sqlite/ | third_party/sqlite/ |
| 루트 build*.cmd | scripts/ 구현을 호출하는 호환 진입점 |

공개 include/resource_collector.h와 include/monitor.h, collector 계산·Windows adapter·identity·GUI·콘솔 원본, schema.sql은 이동 전후 바이트 일치를 확인했다. 실행 스키마의 SQL 문자열도 동일하며 헤더의 출처 경로 주석만 갱신했다. monitor.c의 34개 함수는 내부 helper 호출 이름 및 일부 static 연결 범위 변경을 제외하고 본문을 유지했다. 상세 검증은 [validation](validation.md)에 있다.

공개 C API/자료형, batch/snapshot 소유권, 데이터 단위, DB schema/app ID/집계 버전, 관측창·Baseline 선택·gate·상수는 바꾸지 않았다. DB 재작성이나 migration은 없다.

**수동 빌드 소스 경로는 변경되었다.** 새 디렉터리의 소스 목록과 include 검색 경로를 scripts와 일치시켜야 한다. monitor를 사용하는 수동 빌드에는 storage/monitor_storage.c와 detection/baseline.c가 추가로 필요하다. 테스트·문서 링크·vendor 검증 경로도 새 위치를 따른다. 내부 helper의 새 링크 심볼은 비공개 연결용이며 안정된 외부 API로 취급하지 않는다.

루트 build.cmd, build-monitor.cmd, build-monitor-benchmark.cmd와 기본 출력 이름은 유지한다. 내부 소스 파일 경로에 의존하던 도구에는 위 이동이 호환 영향이 있다. 기존 review 빌드의 특정 Visual Studio 설치 경로를 제거하고 동일 vswhere 탐색을 사용한다. allocation-failure 주입은 기존 함수가 옮겨 간 storage에도 적용해 테스트 의미를 보존했다.

불필요한 vendor ZIP·sqlite3ext.h는 로컬에 보존하고 공개에는 빌드에 쓰는 sqlite3.c/.h와 provenance만 포함한다. 공개 검증기는 소스 SHA256을 확인하고, 원본 ZIP이 있는 로컬에서는 공식 archive SHA3와 내부 파일 일치도 추가 검증한다.
