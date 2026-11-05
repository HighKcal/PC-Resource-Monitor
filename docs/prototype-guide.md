# 변화 사건 프로토타입 실행·평가 안내

현재 Windows C 수집기·집계·개인화 Baseline을 보존하고, 별도 실행파일에 **사건 목록 → 당시 근거 → 다음 확인 → 독립 평가 기록**을 연결했다. 같은 완료창을 개인화와 단순 고정 규칙에 동시에 전달하며 같은 상세 문구와 UI로 표시한다. 실제 여러 날의 사용자 효용·탐지 정확도·장기 성능은 아직 검증하지 않았다.

## 빌드와 첫 실행

Windows x64 MSVC/SDK가 필요하다. 기존 빌드 출력과 다른 이름을 선택할 수 있다. 아래 DB·CSV 이름이 이미 있으면 새로운 이름을 사용한다. 생성·export·replay는 기존 출력 덮어쓰기를 거부한다.

```powershell
.\build-monitor.cmd build-prototype-core
.\build-prototype.cmd build-prototype build-prototype-core
.\build-prototype\test_experiment.exe
.\build-prototype\prototype_cli.exe demo --source build-prototype\demo-01-monitor.sqlite --db build-prototype\demo-01-events.sqlite
.\build-prototype\prototype_gui.exe --db build-prototype\demo-01-events.sqlite --export build-prototype\demo-01-review.csv
```

`demo`는 `ResourceBatch`를 기존 수집 후 처리 경로에 넣는다. 3 UTC 날짜×날짜별24분×서로 다른2개 경로의 준비 자료 뒤에 유효0·일시 초과·상대 증가·지속 초과·회복·CPU 결측·PID 재사용·긴 공백을 만든다. 같은 basename과 다른 경로를 사용한다. 개인화의 날짜·표본 gate를 우회하거나 candidate 결과를 직접 주입하지 않는다. 합성 시각을 빠르게 진행하므로 실행 시간이 실제 다일 학습 기간을 뜻하지 않는다. 사용자 평가 정답이나 사람이 걸린 시간은 생성하지 않는다.

기존 `monitor_gui.exe`, `resource_console.exe` 실행 명령과 collector API·`monitor.h`의 공개 계약·DB 기본 스키마를 유지한다. 기존 실사용 DB를 이 실행의 입력으로 지정하지 않는다. 새 source DB는 기존 monitor 형식에 prototype 짝 식별 표를 추가하고, 새 events DB는 별도 `EXP1` 형식이다. 일반 monitor DB를 prototype 재개 대상으로 지정할 수 없다. 새 파일 이름을 지정해도 이미 존재하는 실사용 DB를 변환하지 않는다.

## 화면에서 확인하는 순서

1. **Events**에서 사건을 선택한다. 최초 관측된 초과 창, 세 번째 연속 초과에서의 활성화, 마지막 관측, 관측된 회복 또는 공백 후 unknown을 구분한다. 시각은 원인 발생 시각이 아니다.
2. 상세의 대상·PID+생성시각·정확한 경로와 완료창 UTC를 먼저 읽는다. 동일명 다른 경로와 재시작 인스턴스를 합치지 않는다. 이후 값·단위·기간·coverage/표본수·과거 대표 수/UTC 날짜/분·중앙값/MAD/상한·정책/참조를 확인한다. 오래된 창은 현재 프로세스 상태를 확인하지 못한 과거 근거다.
   사건 상세는 **Activation evidence**와 **Latest or recovery evidence**를 나란히 설명하며, 같은 결정이면 중복을 생략하고 활성화 근거가 만료됐으면 unavailable로 표시한다.
3. **Both policies / Personalized Baseline / Fixed provisional rule**로 같은 표현의 결과를 비교한다. **All comparison windows**에서는 normal뿐 아니라 learning/unavailable/attention과 후보가 없는 창도 볼 수 있다. 유효0은 숫자0, 미관측은 unavailable로 유지한다.
4. 근거 아래의 **Unknown / Next check**를 읽는다. 앱의 진행 중인 작업·시점, 다른 지표와 작업 완료 후 재관측을 확인한다. 자원 사용만으로 원인·악성·불필요·메모리 누수·물리 디스크 병목을 확정하지 않는다.
5. 평가하려면 `Newly useful / Already known / Unnecessary check / Unsure`와 다음 확인을 선택하고 **Save evaluation**을 누른다. 이해·대상·시각·지표와 **Next check succeeded**는 자기 보고이며 검증된 정답이 아니다. 다음 확인을 실제 시도하지 않았거나 결과를 모르면 성공 항목을 unknown으로 둔다. 초기값은 모두 unknown이다. `Trial ID=ad-hoc`, `Order ID=unassigned`는 비정기 로컬 검토용이며 통제 실험에서는 사전에 배정한 식별자로 바꾼다. Trial ID를 비울 수 없다.

**Start task timer**를 직접 누른 뒤 같은 근거에서 Save한 경우에만 QPC 경과시간을 저장한다. Start 없이 저장하면 시간은 NULL(미측정)이다. 선택·정책·보기 변경으로 다른 근거를 보거나 선택이 사라지면 타이머가 취소된다. 이것은 화면 과제 경과시간이며 개인화의 시간 절약이나 사람의 사고 시간을 자동 측정하지 않는다. 자동화 smoke의 기록은 별도 trial/note로 표시하고 사용자 결과에서 제외한다.

다른 근거를 선택하면 판단·확인 항목·메모도 초기화하고 trial/order ID는 유지한다. 상단의 **LATEST SOURCE RAW**는 마지막 source 회차의 실제 raw행/예상행과 신원·경로·metric 결측 수다. 항목별 수는 서로 겹칠 수 있으며 아래의 과거 완료창 상태와 다르다. source raw가 만료·불완전·불가하거나 replay이면 결측 개수를 NA로 표시한다. source 짝이 존재한다는 것만으로 raw 근거가 완전하다고 판단하지 않는다. 완료창이 아직0개인 경우도 정상으로 표시하지 않는다.

아주 긴 이름·경로는 UTF-8 문자 경계를 지켜 줄여 표시하고 truncation 표식을 붙인다. 전체 문자열이 필요하면 명시적으로 만든 로컬 export에서 확인한다.

평가는 별도 원장에 저장한다. 기존 Ignore/Expected 알림 억제와 연결하지 않으며 Baseline·원시 값·후보 사실·학습 gate를 바꾸지 않는다. 같은 근거를 여러 번 평가할 수 있으므로 분석할 때 반복 노출과 중복 평가를 구분해야 한다.

한 번에 사건200개, 비교창은 정책별200개까지 읽는다. Previous/Next page로 앞뒤를 이동하며, 사건은 가져온 공통 페이지에 정책 필터를 적용하므로 특정 정책의 페이지가 적거나 비어 보일 수 있다. 새로 수집한 내용을 보려면 Refresh를 누른다. GUI worker만 events DB를 열고 복사된 목록을 UI에 넘긴다. 종료는 대기 중인 로컬 작업을 처리하고 DB를 닫으며 UI 메시지 처리를 계속한다. 기존 live GUI의 Pause/Resume은 보존했다. 새 검토 GUI에는 수집 시작·중지 버튼이 없다.

## 로컬 실제 수집과 재개

짧은 동작 확인은 별도 새 파일로 시작한다.

```powershell
.\build-prototype\prototype_cli.exe live --source build-prototype\local-01-monitor.sqlite --db build-prototype\local-01-events.sqlite --count 25 --interval-ms 3000
.\build-prototype\prototype_gui.exe --db build-prototype\local-01-events.sqlite
.\build-prototype\prototype_cli.exe live --resume --db build-prototype\local-01-events.sqlite --count 25 --interval-ms 3000
```

`live`는 실제 collector의 한 회차 → 기존 monitor 집계/Baseline → 같은 완료창의 두 정책 비교 → 사건 저장 순서다. count를 생략하거나0으로 정하면 Ctrl+C까지 수집한다. 기본3초는 처리 **후 대기**라 실제 cadence는 처리 시간을 포함한다. 25회는 약1분의 연결 확인이며3날짜 준비나 장기 정확도 검증이 아니다. 재개는 원래 prototype source 짝을 확인하고 새 세션으로 시작한다. 합성 DB를 실제 수집으로 재개할 수 없다. CLI 종료/관측 중단은 사건을 정상 회복으로 바꾸지 않는다.

GUI는 source를 수집 writer로 열지 않는다. 공정한 UI 비교는 수집 CLI를 종료한 동일한 고정 DB에서 수행한다. GUI refresh의 통계·목록은 여러 읽기로 구성돼 동시 수집 중의 단일 transaction snapshot을 보장하지 않는다. 같은 events DB에 여러 평가 writer를 동시에 띄우지 않는다. DB 접근 실패/잠금/부분 저장 실패는 오류로 보고하고 관측 성공으로 세지 않는다.

실제 수집 로그는 회차별 collect, 전체 prototype ingest, 기존 core store, 둘의 차이인 sidecar 비용을 QPC ms로 표시한다. WorkingSet/PrivateUsage는 이 모니터 프로세스의 bytes다. 종료 요약에는 수집기 생성부터 종료 처리까지의 벽시계·프로세스 CPU초/전체 논리 CPU 용량 대비 비율·process I/O bytes와 종료 처리시간을 기록한다. 실패 값은 NA다. DB open 전 비용·OS 전체 부담·물리디스크 I/O·모든 환경에서의 지연 상한을 측정한 것은 아니다.

## 규칙과 재현 범위

| 항목 | 개인화 Baseline | 비교용 고정 규칙 v1 |
|---|---|---|
| 입력 | 같은 개별 process 완료창, 같은 품질·결측·연속성 | 동일 |
| 상한 | 기존 median+max(3×1.4826×MAD,0.5×median,floor) | CPU25%, WorkingSet512MiB, read/write각10MiB/s |
| 준비 | 60창/3 UTC 날짜/60분; 엄격한 과거만 | 유효한 완료창부터 가능 |
| 지속 | 3개 연속 초과 창 | 동일 |
| 표현·평가 | 공통 상세·다음 확인·독립 평가 | 동일 |

고정 상수는 효과가 검증된 최적값이 아니라 동등한 비교를 위한 사전 정책이다. 개인화가 learning인 시간도 사용자 경험과 비교 가능률의 분모에 포함한다. 고정 규칙의 과거 통계는 문맥이며 그 규칙의 상한 계산에 사용하지 않는다. 현재창은 기존 engine에서 비교 후 학습하며 평가 기록은 학습에 쓰이지 않는다.

```powershell
.\build-prototype\prototype_cli.exe list --db build-prototype\demo-01-events.sqlite --windows --method all --limit 100
.\build-prototype\prototype_cli.exe detail --db build-prototype\demo-01-events.sqlite --decision 1401
.\build-prototype\prototype_cli.exe replay --db build-prototype\demo-01-events.sqlite --out build-prototype\demo-01-replay.sqlite
.\build-prototype\prototype_cli.exe export --db build-prototype\demo-01-events.sqlite --out build-prototype\demo-01-export.csv
```

목록의 실제 decision ref를 detail에 사용한다. demo의 숫자는 코드 변경으로 달라질 수 있다. replay는 입력 events DB를 읽기 전용으로 열어 **복사해 보존한 완료창 근거·품질·연속성·기록 순서**에서 사건을 다시 만든다. 완전한 retained 이력에서는 고정 비교를 다시 계산·검증한다. 만료로 앞부분이 없으면 보존한 당시 결정/사건 요약을 사용한 projection-only임을 표시한다. 개인화 Baseline을 남아 있는 값으로 새로 학습하거나 삭제된 과거를 복원하지 않는다. GUI/통계의 replay 상태를 함께 확인한다. replay에는 실제 새 관측이나 새 사용자 평가가 추가되지 않는다.

source의 raw는1시간, 집계는28일이다. 사건 sidecar의 비교 근거도28일 정책이며 source 창이 없어도 보존한 당시 수치가 있는 동안 설명할 수 있다. 개별 근거 ref가 만료됐으면 unavailable로 처리한다. 영구 보관·완전한 원시 시계열 복원·엄격한 파일 크기 상한을 보장하지 않는다. DB와 WAL/SHM, CSV는 프로세스명·개인 경로·시각을 포함할 수 있으므로 로컬에 보관하고 공개 저장소에 추가하지 않는다.

## Export를 분석하기

CSV는 `record_type,record_id,payload_json` 세 열의 typed record다. metadata(정책·template·origin/replay), operation/gap, window/metric evidence, decision, episode, evaluation을 분리해 보존한다. payload는 CSV quoting을 먼저 해제한 뒤 JSON으로 읽는다. 평가 elapsed NULL은0초가 아니다. 예를 들어 Python 표준 라이브러리만으로 유형별 개수를 확인할 수 있다.

```python
import csv, json
from collections import Counter
with open(r"build-prototype\demo-01-export.csv", encoding="utf-8", newline="") as f:
    records = [(r["record_type"], r["record_id"], json.loads(r["payload_json"]))
               for r in csv.DictReader(f)]
print(Counter(kind for kind, _, _ in records))
```

원본 JSON 키와 버전을 확인하고 window/decision/episode ref로 연결한다. 같은 process 창의4개 metric을 독립된4분의 관측시간으로 합산하지 않는다. 상태별 `metric_window_seconds`는 metric별 창초의 합이며 전체 PC 가동시간이 아니다. label이 없는 구간은 unknown이다. 평가 유용성·자기 보고·이미 앎·억제 기록을 곧바로 객관적 정답이나 accuracy로 바꾸지 않는다.

## 28일 실제 파일럿 준비표 — 아직 실행하지 않은 계획

이번 구현 작업과28일 관찰을 분리한다. 현재28일 파일럿을 실행 완료했다거나 참가자 효용을 측정했다고 보고하지 않는다. 시작 전 사용자가 로컬 보관 위치·대상·기간·삭제/검토일을 정한다. 기본값에 명령행·창 제목·문서명·스크린샷을 추가하지 않는다. 자동 상주·예약·업로드도 시작하지 않는다.

| 시기 | 준비/운영 | 남길 판단 자료 |
|---|---|---|
| 시작 전 | 대상 Windows/동시 process수와 성능 예산, 참가자·trial/order ID, 동등하지만 다른 사례, 도구 학습시간, 실패·중단 기준을 정한다 | 코드/빌드 SHA, 정책·template version, 고정 상수, 수집 interval, 수집 범위·보관 위치/기한 |
| 1~14일 | 준비·정책 개발. 두 정책은 같은 입력을 읽는다. 준비가 안 된 경로와 관측 실패도 기록한다 | metric별 learning/unavailable/판정 가능 창·시간, 비용/DB 증가, 독립 증상 일지, 후보 없는 검토 슬롯 |
| 14일 종료 | 정책·상수·표현·과제·평가 기준을 고정하고 파일로 보존한다 | freeze 일시와 SHA/version, 제외·결측 기준, 사전 결정 기준 |
| 15~28일 | 고정한 정책으로 미래 구간을 평가한다. 이전 이력만 사용하고 현재창 평가 후 학습한다 | 날짜/사용자/사건별 유용함·이미 앎·불필요 확인·unknown, 확인 성공/시간, 놓친 증상과 지연·중단 |
| 종료 검토 | 준비 부족·관측 결측·무증상·탈락도 포함해 해석한다 | 반복되는 오해·잘못된 대상/단위/원인 단정, 두 정책의 조건부 결과와 전체 경험, 지속 투자/축소/보류 판단 |

정책 UI 비교의 순서는 AB/BA 등으로 균형 배정하고, 같은 사례를 외운 효과를 피하려면 서로 다른 동등 사례를 사용한다. 반복 노출은 order/trial/note에 기록한다. 기존 최선 도구 흐름에도 짧은 학습과 적절한 preset을 제공하며 초기 설정시간과 반복 확인시간을 분리한다. 새 사건 UI의 이득과 개인화 후보 선택의 이득을 같은 것으로 계산하지 않는다.

증상 일지는 후보 목록과 독립적으로 정한 시각에 간단히 작성한다. 예: `diary_utc,trial_id,symptom_yes_no_unknown,description_optional`의 로컬 파일을 직접 관리한다. 후보가 없던 슬롯도 사전에 정해 All comparison windows로 검토하고 decision ref를 남긴다. 후보만 평가하면 놓친 사건을 알 수 없다. Ignore/Expected는 정답 라벨이 아니다. UI의 자기 보고와 별도의 검토 정답도 구분한다.

주 지표는 올바른 대상·시점·metric 확인, 적절한 다음 확인, 실제 측정시간, 불필요 확인 episode/관측시간, 알려진 사건의 발견·지연과 비교 가능률이다. 실제 확인된 사건·정답이 부족하면 recall/미탐률/accuracy를 계산하지 않는다. 30% 시간·불필요 확인 감소 같은 연구 보고서의 수치는 팀의 사전 투자 기준 초안이며 이번 결과가 아니다. UI만 도움이 되고 개인화 차이가 없으면 단순 사건 기록 도구로 축소하고, 최선 기존 흐름보다 이득이 없거나 부담이 크면 상주앱 확장을 보류한다.

## 검증 범위

`test_experiment.exe`는 합성/저장/사건/평가/replay 계약을 확인하는 기본 테스트다. 기존 collector/monitor 회귀는 기존 `scripts/test.cmd`로 별도 실행한다. GUI는 headless 테스트와 별도로 선택·경로 구분·정책 전환·페이지·learning/결측/0·평가·타이머·export·종료를 화면에서 확인해야 한다. DPI·접근성·장시간 사용·28일 실제 학습·사용자 효용·부하 상한은 별도 검증 대상이다. 보안 정책 때문에 실행이 차단되면 차단 사실을 기록하고 정책을 변경하지 않는다.
