# core commit 결과 조회 계약

`engine/monitor_source.h`는 prototype과 같은 worker에서 사용하는 내부 계약이다.
기존 공개 `monitor.h`의 API/ABI나 DB schema를 확장하지 않는다. 구현·검증 버전은
[아키텍처](../architecture.md)와 [검증 요약](../validation.md)에 연결한다.

## 참조의 의미와 수명

`monitor_ingest_with_commit_ref`는 기존 ingest 순서로 core를 저장하고 COMMIT 뒤에
snapshot과 참조를 공개한다. 참조는 process에서 재사용하지 않는 engine cookie,
mutation epoch, snapshot sequence, session ID, run ID, 회차 history cutoff를 갖는다.
이 값은 해당 engine이 소유한 DB connection의 commit을 가리키며 직렬화하거나 다른
engine으로 이전하는 식별자가 아니다. 일반 `monitor_ingest`는 ref=NULL로 같은 경로를
사용하며 prototype 전용 source 조회·배열·할당을 수행하지 않는다.

다음 ingest **시도**는 입력 오류·out=NULL·저장 실패를 포함해 이전 참조를 무효화한다.
reset, feedback/record_failure/pair 쓰기 시도, close도 무효화한다. 읽기 전용 stats와
source 조회는 새 commit을 만들지 않는다. cookie는 포화 시 재사용하지 않고 engine
개방을 거부한다. epoch도 wrap하지 않고 참조 발급을 영구 중단한다. ordinary core
저장 성공과 참조를 조회할 수 있는지를 구분해야 한다.

호출자는 원래 반환된 **살아 있고 수정하지 않은 snapshot**을 전달한다. engine은
현재 성공 출력 pointer/rows, sequence, count/completed count, synthetic, UTC·foreground
metadata와 참조 전체를 대조한다. storage reader는 실제 session/run 존재와 DB의
회차·창 metadata를 확인한다. 해제된 snapshot, 닫힌 engine, 임의로 조작한 메모리의
안전성을 보장하는 registry/refcount 계약은 아니다.

## 값과 자원 소유권

`monitor_source_read_committed`의 out은 비어 있는 caller 소유 container다. 기존 결과가
있으면 먼저 `monitor_source_windows_free`로 해제한다. 성공한 배열은 caller가 소유하고
각 항목의 row/name/path/report는 snapshot을 빌린다. snapshot 또는 참조 수명보다 오래
보관하지 않는다. prototype은 바로 sidecar에 값을 복사하고 trim/COMMIT 전에 배열을
해제한다. 배열 해제는 snapshot을 해제하지 않으며 container를 비운다.

완료창0도 committed collection_run 존재를 확인하고 빈 배열을 반환한다. 완료창이
있으면 정확한 개수의 view만 할당한다. 조회 범위는 run/session 및 id>cutoff이고 ID
순서다. count·metadata 불일치, prepare/step 오류, OOM이면 부분 배열을 공개하지 않고
statement와 pending 배열을 정리한다. source reader는 engine 구조체를 보지 않으며
검증된 scalar ref와 opaque store, snapshot만 받는다.

기존 public batch는 중복 PID+생성시각을 허용한다. source view는 이전 prototype의
첫 matching row 선택을 보존하고 duplicate/metadata-mismatch flag를 준다. unique
tuple의 metadata 불일치는 오류다. 중복 tuple의 각 원본 창과 서로 다른 row report가
정확히 대응한다고 주장하지 않는다. 이 기존 한계는 별도 공개 API fixture로 전후
비교하며 새 거절이나 정책 수정과 섞지 않는다.

## 실패와 부분 성공

Track staging은 core 호출 전에 확보한다. core 실패는 snapshot을 공개하지 않으며
기존 rollback/reset 계약을 따른다. core 성공 뒤 source view 조회·할당·복사·sidecar
저장이 실패하면 core commit은 남는다. wrapper는 sidecar를 rollback하고 snapshot과
staging/view를 해제하며 fault 상태와 명시적 부분 실패를 반환한다. reset/resume이
필요하며 같은 batch의 core ingest를 자동 재실행하지 않는다.

이전 wrapper의 core 전 max-window SELECT는 제거됐다. 새 view allocation은 core
commit 뒤에 있으므로 실패 위치가 전부 동일하다고 보지 않는다. 이전에는 source 창을
한 행씩 읽고 바로 sidecar에 썼지만 이제는 전체 view의 조회·검증을 마친 뒤 창을 복사한다.
gap/continuity 작업은 기존 순서로 같은 sidecar transaction 안에 있고, 어느 단계에서
실패해도 sidecar 전체 rollback과 core commit 보존이라는 호출자 결과를 유지한다.
원래 일반 ingest의 회차 cutoff 선택과 commit 순서는 유지한다. error text는
engine/store가 소유하므로 다음 실패 호출 전에 필요하면 복사한다.

## 짝 확인과 읽기 전용 진단

짝 표의 생성·검사와 readonly source connection/statement 수명은 source storage가
관리한다. 일반 monitor DB를 prototype source로 변환하는 API가 아니다. 기존 새
파일 생성·짝 확인·재개 규칙과 서로 분리된 core/sidecar commit을 유지한다.

`MonitorSourceDiagnostics.source_available`은 짝 확인 여부이고 `available`은 최근
회차의 raw 자료 조회 완전성이다. UTC/status/예상 행 수를 읽은 뒤 후속 조회가 실패하면
이미 읽은 필드를 유지하되 available을 성공으로 만들지 않는다. 결측 수0을 자료
조회 성공으로 해석하지 않는다. 여러 readonly 조회는 동시 외부 writer에 대한 단일
transaction snapshot을 보장하지 않는다. 이 비용을 GUI rendering 성능으로 사용하지 않는다.
