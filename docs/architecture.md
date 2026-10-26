# 아키텍처와 공통 계약

## 책임과 의존 관계

| 영역 | 구현 | 의존·검토 경계 |
|---|---|---|
| collector | Windows adapter, 순수 계산 core, 생성시각 재검증 path adapter | 공개 ResourceBatch. SQLite·GUI에 의존하지 않는다. |
| storage | DB open/close, schema, prepared SQL, raw/program, retention, feedback, stats | 공개 monitor.h와 내부 engine 상태를 사용. prepared Baseline 조회는 detection과 공동 계약. |
| detection | Baseline 표본 수신, median/MAD·상한, 준비 gate·지속성 | engine의 현재 창·cutoff와 storage의 prepared query를 소비. 공개 API를 추가하지 않는다. |
| engine | 수집 batch 검증, 트랜잭션, 인스턴스 연결, 관측창, snapshot | collector의 신원 어댑터와 storage helper, detection을 호출. |
| gui | worker loop, UI, snapshot 교환, feedback command | monitor.h/resource_collector.h만 사용. SQLite connection을 직접 공유하지 않는다. |
| app | 콘솔 수집 실행 | resource_collector.h만 사용. |

`monitor.lib`가 필요한 모듈과 SQLite를 함께 정적으로 묶는다. UI와 headless 모두 같은 worker와 engine을 사용한다. 공개 헤더는 include/, 내부 연결은 engine/monitor_internal.h, storage/storage_internal.h, detection/baseline.h에 둔다. 서로의 공개 자료형을 복제하지 않는다.

## monitor.c 분리 결정

기존 34개 함수를 기능별로 이동했다. storage는 DB lifecycle·SQL helper·원시 저장·보관·피드백을, detection은 통계·Baseline 판정을 담당한다. engine은 transaction·save_window·advance_window·ingest와 snapshot을 유지한다. 함수 본문은 내부 helper 이름과 static 연결 범위 외에는 바꾸지 않았다. 공유 MonitorInstance 구조체만 내부 헤더로 이동했다.

완료창 저장은 현재 창의 품질 계산, Baseline 비교, SQL insert, 창 reset의 순서와 rollback에 강하게 결합되어 engine에 남겼다. 이를 폴더에 맞춰 별도 wrapper로 쪼개지 않았다. `save_window`의 SQL/품질 변경은 storage·detection 공동 검토, 관측 순서 변경은 collector·GUI까지 영향 평가한다. 저장 backend와 통계 모듈은 각각 별도 번역 단위로 빌드된다.

## 실행·소유권·실패 흐름

worker가 collect 한 회차를 받아 engine에 동기 전달한다. engine은 batch 시작에 과거 aggregate 최대 ID cutoff를 고정한 transaction에서 raw 기록·관측창 진전·과거 비교·완료창 저장·bounded 정리를 수행한다. 성공 commit 후 새 인스턴스 상태와 독립 snapshot을 공개한다. 실패 시 rollback, 불완전 상태·streak reset, 명시적 오류를 전달한다.

ResourceBatch/문자열은 batch 소유, MonitorSnapshot/문자열은 반환 후 호출자 소유다. snapshot은 batch·engine·DB cursor를 참조하지 않는다. GUI는 잠금 아래 snapshot/복사된 feedback command만 교환한다. connection과 statements는 worker만 사용한다. 종료 후 batch_free/snapshot_free/monitor_close/collector_destroy로 각 소유권을 정리한다.

collector의 실제 QPC 구간과 기록용 UTC는 구분한다. CPU는 전체 논리 CPU 대비, RAM은 working set bytes, I/O는 process B/s다. PID+생성시각, 필드 validity, 결측 0 금지, 첫 표본·실패·역행·긴 공백 무효화는 [collector 계약](contracts/collector.md)을 따른다.

Baseline은 현재 표본을 넣기 전에 비교한다. strict-past 정밀 start와 회차 cutoff를 모두 사용하고 날짜별 최대 64개의 서로 다른 종료 분 대표를 선택한다. [저장](contracts/storage.md)과 [판정](contracts/detection.md) 계약을 함께 적용한다. DB schema/app ID, 공개 헤더, 관측창·통계 상수는 이번 이동에서 유지했다.

## 선택적 확장 경계

자원 이상, 역할 근거, 별도 보안 분석, AI 설명은 각각 별도 결과다. 단일 안전/불필요 판정으로 합치지 않는다. 외부 탐지기의 원래 label·검사 범위·버전을 보존하며 no finding은 전체 안전 보장이 아니다. 사용자 자원 피드백은 보안 결과나 측정 사실을 덮어쓰지 않는다.

이하 내용은 논리적 설계이며 현재 ResourceData·공개 C 구조체·DB 테이블·실제 adapter에 추가하지 않았다.

- 대상 ref는 관측 origin/session과 PID+생성시각을 포함한다. 파일 분석에는 경로 외 확인된 파일 객체·콘텐츠 버전, 취득 시각·방법·불확실성을 별도로 기록한다. 같은 경로만으로 교체된 파일에 결과를 승계하지 않는다.
- 요청은 protocol/schema version, request ID, 종류, target ref, 요청시각, 근거 ref/version, 동의 범위, deadline/cancel을 가진다. 비동기 요청은 허용된 최소 scalar/문자열을 복사해 소유하며 batch 포인터를 보관하지 않는다.
- 결과는 request/result ID, 실제 target ref, provider/engine/rules/model 버전, 시작·완료시각, 근거·원문 label, 실행 상태, expires_at·오류를 가진다. 공급자가 정의한 scale이 없는 신뢰도는 NULL이다.
- not_requested, pending, running, failed, cancelled, inconclusive, completed, stale/expired를 구분한다. 미실행·실패·불명·만료를 정상/안전으로 바꾸지 않는다.
- 반환 시 PID+생성시각과 필요한 파일 버전을 재검증한다. 종료·PID 재사용·파일 교체·늦은 중복 결과는 현재 행에 붙이지 않는다. 과거 결과는 그 당시 대상으로 보존하거나 stale 표시한다.
- AI 설명에는 참조한 run/window·통계 정책 버전·역할/보안 result ID를 연결한다. 문장은 원자료의 authoritative source가 아니며 관측사실·가능 원인·추정을 구분한다.
- 장시간 선택 작업은 core 수집 worker를 막지 않는 별도 비동기 경계를 사용한다. timeout·취소·미설치·오류가 core 수집과 UI를 중단시키지 않아야 한다. IPC/CLI/DLL 선택은 실제 외부 프로젝트의 허용된 공개 interface를 확인한 뒤 정한다.
- 메타데이터·프로세스명·탐지 결과·AI 문장은 신뢰하지 않는 데이터다. 길이·형식·encoding을 검증해 텍스트로 표시하고 포함된 명령을 실행하지 않는다.

외부 서비스·전송 데이터는 별도 사용자 동의 범위에서 결정한다. 전체 경로·파일본문·파일 업로드를 자동 포함하지 않는다. 현재 실제 AI/API 호출, 외부 프로젝트 탐색, 검사 VM 변경·실행, 자동 프로세스 종료·격리는 없다. 외부 탐지 프로그램의 경로와 읽기 허용 범위·API·결과 규격이 제공되지 않아 호환성은 미확인이다.
