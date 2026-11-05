# 변화 사건 비교 프로토타입 계약

변화 사건의 수집·비교·설명·별도 평가를 검증하는 로컬 프로토타입이다. 아래 임계값·모듈 구성·사건 종료 규칙은 연구에서 실증된 결론이 아닌 이번 프로토타입의 잠정 설계다.

## 목적과 호환 경계

가설은 같은 근거와 화면에서 개인화가 고정 규칙보다 불필요한 확인을 줄이는지, 사건 설명이 대상·시점·지표 확인을 돕는지다. 개인화의 우위를 완료 조건으로 삼지 않는다. 원래 collector, monitor.h의 공개 계약, engine/storage/detection의 관측·판정 의미, monitor GUI와 기존 DB 스키마를 유지한다. 별도 실행파일은 같은 core ingest 경로의 내부 commit 참조 변형을 호출하며 새 실험 전용 원본 DB와 별도 실험 DB를 사용한다. 실사용 DB의 변환·삭제·업로드는 없다.

새 include/experiment.h는 모듈 전용 계약이다. worker가 engine과 DB를 소유한다. UI에는 독립 소유 복사본만 전달한다. 원본 수집 transaction이 성공한 뒤 실험 기록을 저장한다. 후자의 실패는 별도 오류로 드러내고 재현의 공백으로 처리하며 성공한 원본 측정을 덮지 않는다. 평가는 Baseline·Ignore/Expected와 분리한다.

## 책임·소유권과 core 경계

| 파일 | 책임과 입력·출력 |
|---|---|
| `experiment.c` | lifecycle, Track 사전확보, core commit→sidecar transaction, fault/reset, commit 뒤 Track/snapshot 공개를 조율한다. |
| `experiment_capture.c` | batch와 이전 Track으로 gap·다음 Track을 준비하고 검증된 source view를 sidecar 값으로 복사한다. staging은 caller 소유다. |
| `experiment_store.c` | sidecar schema/open/검사, SQL·오류·문자열 helper, operation/window/metric 저장. Track을 수정하지 않는다. |
| `experiment_reducer.c` | 복사된 보고서·gap·replay seed에서 decision/episode/reduction을 갱신한다. core 상태·DB에 접근하지 않는다. |
| `experiment_history.c` | 목록/상세, 평가, 통계, 설명, export. core 진단은 typed source 결과를 받아 공개 stats로 매핑한다. |
| `experiment_replay.c` | 원본 evidence를 새 DB에 보존하고 순서대로 reducer를 적용한다. 요약만 남은 경계 사건은 마지막에 복원한다. |
| `experiment_retention.c/.h` | DB·UTC·오류버퍼만 빌려 bounded 정리한다. caller가 transaction/rollback을 소유한다. |

`experiment_internal.h`는 위 prototype 구현과 test adapter만 사용하는 private context다.
coordinator가 core, sidecar connection, Track, source/pair 문자열의 최종 수명을 소유한다.
store/reducer/history/replay는 동기 호출 동안 필요한 context와 자료를 빌린다. 임시
statement는 생성한 함수가 해제하고 실패 transaction의 rollback은 caller가 결정한다.
retention은 이 context를 포함하지 않는다. 물리적인 책임 분리를 완료했지만 sidecar
context를 책임별 여러 opaque 객체로 쪼갠 것은 아니다. transaction/오류/metadata 공유는
남아 있으며 store가 Track을 변경하거나 reducer가 core 내부에 접근하는 용도로 쓰지 않는다.

prototype은 engine private header나 core DB handle을 사용하지 않는다. 기존 공개
open/close/reset과 [source facade](source.md)만 사용한다. facade의 commit ref는 같은
engine의 현재 live snapshot과 결합하고 실패한 다음 ingest 시도에도 무효화된다.
source 배열은 core commit 뒤에만 할당·조회하고 즉시 복사한 뒤 해제한다. 이 단계가
실패해도 core를 다시 ingest하거나 rollback됐다고 보고하지 않는다. 기존 pre-core
cutoff 조회 제거와 새 postcommit 배열 OOM은 서로 다른 실패 위치 변화다.

중복 PID+생성시각 입력은 기존 첫 matching snapshot 행 선택을 보존한다. source의
ambiguity flag는 진단이며 새 거절 규칙이 아니다. 서로 다른 중복 row의 값을 각 source
창에 정확히 연결하는 것은 기존 한계로 남고, 정상 unique tuple의 metadata 대조와
구분해 검증한다. 전후 근거는 [검증 요약](../validation.md)에 있다.

## 동일 입력과 정책

두 비교기는 같은 PID+생성시각, 경로, 실제 60~75초 완료창, metric 값·단위·품질을 사용한다. 개인화는 기존 strict-past와 회차 cutoff, 최근14 UTC날짜·일64 종료분 대표, median/MAD+floor, 60창/3날짜/60분 gate, 3창 지속 결과를 그대로 보존한다. 새 비교기가 개인화의 학습 eligibility나 표본 선택을 바꾸지 않는다.

고정 규칙 v1은 CPU 25%(전체 논리 CPU 용량), RAM WorkingSet 536870912 bytes, read/write 각각 10485760 B/s 초과가 3창 연속일 때 후보가 된다. 연구 비교용 초기 상수이며 튜닝된 최적값·외부 제품 구현이 아니다. 개인화 learning은 미준비로 남고 고정 규칙의 판정 가능 여부와 별도로 기록한다. 두 방식에 같은 과거 통계·품질·설명 형식을 제공한다.

완료창만 보면 창 중간 metric 결측에 따른 streak reset을 잃을 수 있다. wrapper는 매 batch의 identity·metric validity·관측간격·비증가 시각·인스턴스 부재와 reset/수집실패를 전달한다. 고정 규칙도 같은 연속성 경계를 적용한다. 결측은 0이 아니며 유효한 0은 회복 근거가 될 수 있다.

## 사건과 근거

사건 key는 실험 origin/session+PID+생성시각+metric+method다. 같은 이름·경로의 다른 실행을 합치지 않는다. 첫 관측 초과창을 시작으로 기록하고 세 번째 연속 초과창에 사건을 활성화한다. 이후 같은 지속 변화는 같은 사건을 갱신한다. 이는 원인이 실제 발생한 시각이 아니다.

품질이 충분하고 비교 준비가 된 창에서 상한 이하를 관측했을 때만 관측상 회복으로 종료한다. 결측·공백·learning·재시작·부재·수집 종료는 판단 불가로 끝내며 해결로 표시하지 않는다. 늦거나 중복된 입력은 현재 사건에 잘못 부착하지 않으며 재처리의 순서와 중복 정책을 명시한다.

완료창의 stable source run/window ref, 비교 cutoff, PID/creation/path, 시작·끝 UTC, 실제 duration, current·coverage·valid_samples, state/reason/streak, n/days/minutes/median/MAD/threshold, method/policy/template/schema version, 합성/실제 구분을 보존한다. 원본 측정과 개인화 판정을 덮지 않는다. 사건 projection과 별도로 불변 비교 근거를 기록한다. 후보가 없는 창과 미준비·결측도 내보낼 수 있어야 한다.

설명은 관측 사실 / 비교 차이 / 확인 불가 / 다음 확인으로 구분한다. 작업 상태·변화 시점·다른 자원 지표·작업 종료 후 재관측을 안내한다. process 이름·background·높은 수치로 원인·필요성·악성을 확정하지 않는다. 완료창과 현재 raw값을 혼동하지 않는다.

## 기록·재생·평가

순서 보존 replay는 보존된 비교 입력·당시 개인화 결과·연속성 경계로 사건을 재생한다. 이력의 앞부분이 온전히 있으면 고정 규칙을 다시 계산해 당시 결과와 대조하며 불일치는 오류로 처리한다. 앞부분이 보관 정책으로 만료되면 당시 결정과 사건 경계 요약을 사용하는 projection-only 모드로 명시한다. 두 경우 모두 개인화 Baseline을 처음부터 다시 학습한 것과 구분한다. 정책 비교를 새로 튜닝하려면 별도 개발 자료와 새 실험 ID를 쓴다. 기존 데이터에 정책을 소급 적용해 원래 결과를 고치지 않는다.

실험 DB에는 schema와 정책 상수를 고정하고 source/sidecar 연결을 검증한다. 처음 생성하는 원본 DB 외 임의의 기존 monitor DB를 실험 DB로 바꾸지 않는다. 재생·export 대상은 기존 파일 덮어쓰기를 거부한다. 원본 raw1h/summary28d를 유지하고 실험 evidence 기본 보관 범위도 28일로 제한한다. 보존된 집계 설명과 만료된 원본의 정확 재계산 가능성을 구분한다. 보존 연장은 이번 기본 동작이 아니다.

보관 정리는 `prototype/experiment_retention.c`가 맡고 connection과 transaction은 기존 coordinator가 소유한다. 28일 cutoff와 evaluation/operation/reduction/episode 각각 최대 1,000 root 삭제를 유지한다. cascade 전체 행 수나 실행 시간의 상한은 아니다. 남은 decision/reduction/evaluation이 참조하는 episode와, 남은 평가의 associated decision 창·종료 gap을 보호한다. 삭제 backlog 때문에 필요한 근거가 cutoff를 일시적으로 넘겨 남을 수 있으나 평가와 만료 연속성도 bounded 정리하여 반복 호출에서 진전한다. episode의 activation/last decision 숫자는 만료 가능한 요약 식별자이며 그 값만으로 모든 과거 창을 영구 보관하지 않는다.

retained replay에서 재생 decision이 남지 않은 boundary episode는 원본 요약의 state/ended/이유를 보존한다. 이를 무조건 active로 초기화한 최종 결과로 남기지 않는다. 남은 결정으로 재구성하는 사건은 기존 경로로 재생하고, 요약만 남은 사건은 재생 transaction의 마지막에 복원하여 더 이른 gap이 결과를 바꾸지 않도록 한다. 이 동작과 평가 고아·backlog 수정의 별도 재현은 [검증 요약](../validation.md)에 있다. 기존 schema/API와 full replay의 고정 비교기 대조는 유지한다.

평가 기록은 usefulness/이미 알고 있음/불필요 확인/불확실, 대상·시점·지표 이해, 다음 확인과 성공 여부, 표시 순서·trial, 선택적 실제 타이머·메모를 다룬다. 타이머는 사용자의 명시적 시작과 저장 사이 QPC 경과만 기록하며 UTC와 분리한다. 측정하지 않은 시간은 NULL, 정답이 없는 구간은 unknown이다. 프로그램 자체의 동작 검증을 인간 평가로 기록하지 않는다.

GUI는 사건 목록→선택→동일 형식 근거→다음 확인→평가 저장을 제공한다. 전체 비교창 보기로 learning/unavailable/후보 없음도 확인한다. 완료창을 만들지 못하는 접근·신원·경로 실패는 별도 최근 source 회차 진단으로 보여 주며 과거 사건 근거와 구분한다. 신원을 확인하지 못한 행이 연속되는 것은 실행 인스턴스가 매번 사라지는 사건이 아니다. 실제·합성 및 오래된 기록을 명시한다. 비교 실험은 수집을 멈춘 동일 DB에서 수행한다. 여러 읽기로 구성된 GUI refresh는 동시 수집의 단일 transaction snapshot을 보장하지 않는다. 표시 순서는 균형 배정하고 동등한 다른 사례를 사용하여 동일 사건 재노출의 학습 효과를 분리한다.

28일 실제 평가의 앞14일은 준비/개발, 뒤14일은 정책을 고정한 전진 평가다. 후보와 독립된 증상 일지와 사전에 정한 무후보 구간 검토를 포함한다. 전체 경험과 판정가능 구간을 함께 보고한다. 확인 시간·불필요 반복·다음 확인 성공을 사용자/날짜/사건 단위로 요약하며 정답 없는 accuracy를 만들지 않는다. 이번 구현 세션은 다일 효용을 검증하지 않는다.

## 수정 소유권과 검증

- engine_implementer: include/experiment.h, prototype/, tests/prototype/test_experiment.c.
- ui_implementer: gui/experiment_gui.c, app/experiment_cli.c, 새 prototype 빌드, README, docs/prototype-guide.md, 시연 입력 생성.
- reviewer: tests/review-prototype/, docs/prototype-review.md, 별도 빌드 출력만.
- main: 이 계약, WORK_LOG.md, 통합 검증·완료 판단과 성능/실행 증거 정리.

기본 경계 시험은 정상·일시 급등·지속 변화·학습 부족, 같은 이름/다중 인스턴스/PID 재사용/재시작, 결측/0/공백/늦은 결과, 사건 갱신/종료/근거, 양 방식 독립, 시간 누출, 평가/export/replay, 기존 회귀다. 실제 수집 연결과 GUI 조작은 별도로 증거를 남긴다. 앱 제어 차단은 우회하지 않으며 콘솔 통과를 GUI 검증으로 바꾸지 않는다. 비용은 기능 전후 처리시간·메모리·DB/WAL·실제 간격을 같은 규모에서 관찰한다.
