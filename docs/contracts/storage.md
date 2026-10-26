# Storage 계약

`storage/monitor_storage.c`는 DB 개방·검사·prepared statement, 경로별 program, raw 기록, 보관 정리, 피드백, 통계를 담당한다. [monitor_schema.h](../../storage/monitor_schema.h)가 실행 스키마이며 [schema.sql](../../storage/schema.sql)은 같은 스키마의 SQL 문서다. 공개 API는 [monitor.h](../../include/monitor.h), 내부 helper는 `storage/storage_internal.h`다.

## 데이터와 트랜잭션

스키마 `user_version=1`, `application_id=1297042993`, 집계 버전 1을 유지한다. `metadata`, `session`, `program`, `collection_run`, `raw`, `aggregate_window`, `aggregate_metric`, `feedback`을 사용한다. UTC는 SQLite signed INTEGER에 담기는 FILETIME ticks다. 범위 초과 입력은 거부한다.

한 worker만 한 connection을 사용한다. `engine/monitor.c`가 한 batch의 `BEGIN IMMEDIATE` → raw·집계·정리 → `COMMIT`을 소유한다. 실패 시 모두 rollback하고 미완료 관측창과 지속성 상태를 버린다. 저장 실패를 성공 학습으로 표시하지 않는다. `save_window`의 SQL 쓰기는 관측창·판정과 연결된 engine 통합부에 남아 있으므로 그 부분은 storage·detection 공동 검토 대상이다.

WAL, synchronous FULL, foreign keys ON, trusted_schema OFF, busy timeout 250ms를 사용한다. timeout이 있어도 BUSY는 가능하다. `main.cache_size=-16384`는 연결별 약 16MiB 페이지 보관 목표이며 전체 process 메모리 상한이 아니다. 이 설정은 Baseline 결과를 재사용하는 캐시가 아니다.

## 식별과 누출 방지

collector가 제공한 PID+생성시각을 같은 핸들에서 재검증한 UTF-8 전체 경로가 program key다. 정확한 문자열 BINARY 키이며 case·alias를 합치지 않는다. 경로 확인 실패는 program NULL과 결측 상태이며 다른 프로그램과 섞지 않는다. 같은 경로의 파일 교체·역할 혼합·버전 구분은 현재 지원하지 않는다.

한 회차 시작에 과거 aggregate 최대 ID인 cutoff를 고정하고 모든 인스턴스 비교에 사용한다. Baseline 조회는 `end_utc < 현재 창의 정밀 start`와 `id <= cutoff`를 모두 지킨다. 현재 회차의 다른 인스턴스에서 방금 저장한 창도 비교에 섞이지 않는다. 상세 선택은 [Baseline 계약](detection.md)에 있다.

## 보관·개인정보

원시 1시간, 집계 및 run/session 28일, 만료 피드백은 만료 후 7일을 기준으로 회차마다 제한된 수만 삭제한다. 중지 기간에 쌓인 backlog나 SQLite 파일 크기가 즉시 줄어드는 것을 보장하지 않는다. Baseline 조회 범위는 현재 시작 날짜 포함 14 UTC 날짜다.

DB는 로컬 디스크 UTF-8 경로만 허용한다. 알 수 없는/미래 스키마나 real/SYNTHETIC 혼용은 migration 없이 거부한다. GUI는 connection을 사용하지 않는다. 실제 DB와 WAL/SHM, process 경로·목록은 Git에 올리지 않는다. 사용 중 DB의 WAL/SHM을 따로 삭제하지 않는다. 백업은 앱 종료 후 보존하거나 [SQLite Backup API](https://www.sqlite.org/backup.html)를 사용하는 도구로 수행한다.

스키마 변경에는 기존 DB 거부·이관·다운그레이드 정책과 관련 담당 검토가 필요하다. 이번 구조 정리는 스키마나 데이터 의미를 바꾸지 않았다.

공식 근거: [WAL](https://www.sqlite.org/wal.html), [Transactions](https://www.sqlite.org/lang_transaction.html), [busy timeout](https://www.sqlite.org/c3ref/busy_timeout.html), [cache_size](https://www.sqlite.org/pragma.html#pragma_cache_size). 의존성 출처·무결성은 [PROVENANCE](../../third_party/sqlite/PROVENANCE.md)를 따른다.
