# 구조 변경과 호환 영향

이번 제출에는 초기 폴더 정리 이후 완료한 성능 개선, GUI·보관 결함 수정, 백엔드 경계 분리와 한국어 설명 주석을 함께 포함한다. 현재 코드는 공개 API/ABI·단위·소유권과 기존 DB 형식을 유지한다. DB 재작성이나 migration 단계는 없다.

| 초기 내부 구조 | 현재 구조 |
|---|---|
| 공유 `engine/monitor_internal.h` | engine만 사용하는 `monitor_engine_private.h`, 값 단위 `monitor_window.h` |
| storage의 engine/instance 직접 접근 | opaque `MonitorStore`, `monitor_store.h` 값 입력과 오류 반환 |
| engine DB/SQL을 보는 prototype | `engine/monitor_source.h` commit ref/view 계약 |
| Baseline 안의 SQL·캐시·정책 | `storage/monitor_history.c`, `detection/baseline_history.c`, `baseline.c` |
| 큰 `prototype/experiment.c` | coordinator와 capture/store/reducer/history/replay/retention 별도 TU |
| `monitor.h`의 공통 결과 선언 | 가벼운 `monitor_types.h`; 기존 `monitor.h`에서 재노출 |
| 개별 빌드의 수동 TU 목록 | `scripts/build-sources.cmd`와 공통 컴파일 helper |

삭제된 `monitor_internal.h`·`storage_internal.h`는 안정된 공개 API가 아니었다. 수동으로 제품 소스를 링크하던 도구는 공통 목록과 include 경로를 따라야 한다. `MonitorInstance` 전체나 DB 포인터를 다른 모듈에 넘기는 방식으로 대체하지 않는다. 시험 전용 접근은 `tests/support/`로 한정한다.

루트 `build.cmd`, `build-monitor.cmd`, `build-monitor-benchmark.cmd`와 기본 출력 경로를 유지하고 `build-prototype.cmd`를 제공한다. 새 checkout은 [개발 명령](development.md)으로 모든 모듈을 다시 컴파일한다. 과거 실행물·설치 경로는 필요 없다. `storage/monitor_schema.h`는 빌드 입력으로 포함하며 `schema.sql`의 SQL과 일치시킨다.

의도적으로 바뀐 결함 결과는 접수한 GUI 명령의 결과 누락 방지, 새 snapshot 전 성공 표시 유지, 보관 후 참조·평가·retained replay 종료 상태 정합성이다. 이 이전의 잘못된 결과를 기능 동등성의 기대값으로 삼지 않는다. 그 밖의 관측·학습·판정 정책과 기존 최적화는 보존했다. 검증 범위와 비용은 [validation](validation.md)에 구분했다.

SQLite는 빌드에 쓰는 `sqlite3.c/.h`와 provenance를 포함한다. 미사용 ZIP·`sqlite3ext.h`, 설치 실행물, 실사용 DB와 과거 원시 검증 자료는 로컬에 남긴다. vendor 검증은 공개 소스 해시로 동작하고 원본 archive가 있는 환경에서만 추가 확인한다.
