# 빌드와 검증 명령

모든 명령은 checkout한 저장소 루트의 PowerShell 기준이다. Windows x64, Visual Studio C++ desktop workload와 Windows SDK가 필요하다. `scripts/setup-msvc.cmd`는 `vswhere`로 MSVC를 찾고 이미 준비된 x64 환경을 유지한다. Python 3는 확장 프로필과 진단에만 필요하다.

## 기본 빌드와 시험

```powershell
.\build.cmd
.\build-monitor.cmd
.\build-prototype.cmd build-prototype build-monitor
.\scripts\test.cmd
.\build-prototype\test_experiment.exe
python -X utf8 tests\review-monitor\verify_vendor.py
```

기존 출력·실사용 DB를 건드리지 않으려면 새 출력 이름을 전달한다. 빌드 스크립트는 소스를 다시 컴파일한다.

```powershell
.\build.cmd build-check-collector
.\build-monitor.cmd build-check-monitor
.\build-prototype.cmd build-check-prototype build-check-monitor
.\scripts\test.cmd build-check-collector build-check-monitor
.\build-check-prototype\test_experiment.exe
```

기본 묶음은 계산·Windows 수집·표시 변환·monitor 통합·grouping·prototype이다. `test_windows`는 실제 API와 자신이 만든 자식 프로세스를 사용한다. 시험이 만든 합성 DB는 출력/임시 경로에 두고 개인 DB를 입력으로 사용하지 않는다. 정책 차단은 실패·미실행으로 기록하며 보안 정책을 바꾸지 않는다.

## 전체 링크와 선택 회귀

```powershell
python -X utf8 scripts\test-profiles.py --profile build --output build-check-profiles
python -X utf8 scripts\test-profiles.py --profile all --built-from build-check-profiles --output build-check-regression
```

첫 명령은 22개 빌드 명령을 직렬 수행하며 실행 파일은 실행하지 않는다. 두 번째는 기능·정책·실패·보관/replay·숨김 GUI 회귀를 실행한다. `basic`, `policy`, `failure`, `prototype`, `gui`로 범위를 좁힐 수 있다. `--built-from` 없이 실행 프로필을 요청하면 먼저 전체 빌드한다. 출력은 **존재하지 않는 새 경로**여야 한다. 입력/산출물 SHA-256이 달라지면 재사용을 거부한다. [프로필 상세](../tests/README.md)를 따른다.

공통 대상은 `scripts/build-sources.cmd`, 컴파일은 `compile-sources.cmd`와 `compile-prototype-fixture.cmd`가 관리한다. 새 제품 TU는 normal/injected/benchmark 목록과 연결 검사를 함께 갱신한다. 독립 oracle의 기대값을 제품 함수로 바꾸지 않으며 allocator·SQL 주입은 실제 hit를 확인한다. `tests/support/`가 opaque 상태의 시험 접근을 한정한다.

기본 프로필에는 과거 소스나 설치 실행물이 필요 없다. GUI의 과거 기대 실패 재현만 `tests/phase1-gui/build.cmd` 네 번째 인수 `historical-before`로 선택한다. 해당 보존본은 공개본에 포함되지 않으며 현재 source로 대신하면 과거 재현이 아니다.

## 실행과 로컬 데이터

```powershell
.\build\resource_console.exe --self --count 3 --interval-ms 3000
$gui = (Resolve-Path .\build-monitor\monitor_gui.exe).Path
$db = Join-Path $PWD 'build-monitor\synthetic-01.sqlite'
Start-Process -FilePath $gui -ArgumentList ('--headless --synthetic --db "{0}" --count 23 --interval-ms 100' -f $db) -WorkingDirectory $PWD.Path -Wait
python -X utf8 tests\monitor_inspect.py $db
```

주 GUI 실행 파일은 headless·synthetic에도 UAC 승격을 요구한다. 합성 통합은 실제 저장·집계를 거치지만 여러 날의 실제 사용이나 화면 상호작용을 검증하지 않는다. 고정 표시만 확인하는 `--ui-demo`와 구분한다. 실제 GUI 조작은 [화면 안내](native-ui.md), 사건 CLI는 [prototype 안내](prototype-guide.md)를 따른다.

DB/WAL/SHM·프로세스명·경로·진단 출력은 로컬 자료다. 사용 중인 DB 파일을 개별 삭제하거나 실사용 DB로 fixture를 실행하지 않는다.

## 선택 성능 도구

```powershell
.\build-monitor-benchmark.cmd build-monitor\monitor.lib build-check-benchmark
$bench = (Resolve-Path .\build-check-benchmark\benchmark_monitor_batch.exe).Path
New-Item -ItemType Directory -Path .\build-check-benchmark\run\build-monitor
Push-Location .\build-check-benchmark\run
& $bench --case all --phase after-cache --instances 500 --days 14
Pop-Location
```

성숙 이력을 생성하는 비용이 있으므로 제출 준비의 필수 시험은 아니다. `--phase after-cache`는 이 도구가 허용하는 출력 이름이며 비교 전후를 자동 판정하는 옵션이 아니다. 전체 pipeline의 한 조건은 `build-check-benchmark\benchmark_pipeline.exe --out build-check-benchmark\initial-mixed --mode experiment --case mixed --instances 500 --days 0 --schedule sync`로 실행한다. `--days 14`는 성숙 이력, `--mode core`는 core 단독, `--case same|mixed|distinct`는 경로 분포다. 출력 prefix는 새 이름을 사용한다.

실행은 한 담당자가 다른 빌드·부하와 겹치지 않게 한다. 버전 비교는 같은 소스·라이브러리 조합, 입력, 권한, DB/캐시 조건을 기록하고 일반 회차와 완료 회차를 나눈다. 결과 digest·메모리·DB/WAL·실제 다음 QPC 간격을 함께 비교한다. 빌드 성공이나 UI 조회가 섞인 측정을 GUI 성능 합격으로 사용하지 않는다.

## 기능별 수정 위치

| 변경 목적 | 시작 파일과 함께 확인할 계약 |
|---|---|
| 수집 단위·실패·신원 | `collector/resource_collector.c`, `resource_core.c`, `include/resource_collector.h`; 수집 계약·계산/Windows 시험 |
| 관측창 품질·reset | `engine/monitor_window.c`; DB 없는 window fixture와 통합 회귀 |
| 저장값·statement·스키마 | `storage/monitor_storage.c`, `monitor_store.h`, `monitor_schema.h`, `schema.sql`; transaction·호환성 |
| 과거 날짜 선택 | `storage/monitor_history.c`, `detection/monitor_policy.h`; exact-start/cutoff·minute 독립 oracle |
| 회차 재사용 | `detection/baseline_history.c`; 키·상한·수명·부분 결과 미공개·OOM fallback |
| 학습·통계·지속성 | `detection/baseline.c`; 저장 선택 조건과 판정 계약 |
| commit 결과 전달 | `engine/monitor_source.h`, `engine/monitor.c`, `storage/monitor_source.c`; ref/snapshot 수명·부분 성공 |
| 변화 사건 | `prototype/experiment_{capture,store,reducer,history,replay,retention}.c`; `experiment.c`의 조율·Track 소유권 |
| 그룹 소속·합계 | `grouping/`; 원본 snapshot 불변·부분합·세션 수명 |
| GUI 명령·성공 표시 | `gui/monitor_gui.c`; 접수 용량·응답과 overlay 수명·worker/SQLite fixture |
| 화면 구성·단위 | `gui/native_*.h`, `gui/monitor_gui.c`; 현재/완료창·GB/MB/s·NA 표시 |

내부 구조체는 공개 계약이 아니다. engine/store private을 새 제품 소비자에게 노출하지 않는다. prototype의 제한된 private context와 GUI의 전역 상태는 아직 남은 결합이며 [아키텍처](architecture.md), [source 계약](contracts/source.md)을 먼저 확인한다.
