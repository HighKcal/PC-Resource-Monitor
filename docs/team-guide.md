# 영역별 필독 문서와 개발 시작점

## 모두가 먼저 읽을 문서

1. [README](../README.md): 현재 기능, 폴더, 빠른 빌드와 한계.
2. [CONTRIBUTING](../CONTRIBUTING.md): Fork → Branch → PR → 동기화 절차와 공통 변경 규칙.
3. [architecture](architecture.md): 모듈 경계, 데이터 흐름, 트랜잭션·소유권.
4. [development](development.md): 재현 가능한 빌드·테스트·합성 검증 명령.
5. [validation](validation.md): 이번 구조 변경에서 실행한 검증과 남은 미검증 사항.

## 영역별 추가 필독

| 영역 | 읽을 문서·공개 규격 | 코드 시작점 / 연결 검토 |
|---|---|---|
| collector | [수집 계약](contracts/collector.md), [resource_collector.h](../include/resource_collector.h) | `collector/`, `app/console.c`, `tests/collector/`. 신원 어댑터는 `collector/monitor_identity.c`; 저장·GUI가 소비하는 단위·유효성 변경은 전원 확인. |
| storage | [저장 계약](contracts/storage.md), [schema.sql](../storage/schema.sql), [SQLite 출처](../third_party/sqlite/PROVENANCE.md), [monitor.h](../include/monitor.h) | `storage/`. `engine/monitor.c`의 batch 트랜잭션·원시/완료창 기록 연결부와 detection의 SQL 선택 조건을 함께 확인. |
| detection | [Baseline 계약](contracts/detection.md), [저장 계약](contracts/storage.md), [monitor.h](../include/monitor.h) | `detection/`, `tests/integration/`, `tests/performance/`. 관측창 품질·cutoff는 engine, SQL 준비와 인덱스는 storage에 있으므로 두 영역과 공동 검토. |
| gui | [GUI 계약](contracts/gui.md), [monitor.h](../include/monitor.h), [수집 계약](contracts/collector.md) | `gui/monitor_gui.c`. snapshot 수명, now/완료창 분리, 결측·학습·STALE·오류 표시, 피드백 전달을 우선 확인. |

이 표는 영역별 책임과 검토 경로를 정한 것이다. 실제 GitHub 사용자명은 확인되지 않았으므로 계정·CODEOWNERS를 추정하지 않는다.

## 첫 기여 전 확인

자신의 Fork를 Clone한 뒤 [development](development.md)의 기본 빌드와 세 테스트를 먼저 실행하고 환경에 따른 차이를 기록한다. 이어서 담당 계약과 코드를 읽고 작은 기능 브랜치에서 변경한다. 공통 변경에는 관련 담당자의 검토를 요청한다.

기존 구조에서 넘어오는 사람은 [migration](migration.md)을 추가로 읽는다. 과거 원본 연구·작업 로그·개인 측정 증거는 공개본에서 제외했다. 현재 팀 개발에 필요한 계약과 검증 해석은 위 문서에 정리했다.
