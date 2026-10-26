# 빌드와 검증 명령

Windows x64, Visual Studio C++ desktop workload, Windows SDK가 필요하다. Python 3는 아래 선택 진단·vendor 검증에만 필요하다. 모든 기본 명령은 저장소 루트의 PowerShell 기준이다. scripts/setup-msvc.cmd는 vswhere를 사용하며 이미 x64 Native Tools 환경이면 이를 유지한다.

## 기본·클린 빌드

```powershell
.\build.cmd
.\build-monitor.cmd
.\scripts\test.cmd
```

기존 산출물에 의존하지 않는 확인은 존재하지 않는 출력 폴더 이름을 골라 수행한다. 빌드 스크립트는 해당 소스를 매번 컴파일한다. 기존 자료를 지우는 clean 명령은 필요하지 않다.

```powershell
.\build.cmd build-clean-collector
.\build-monitor.cmd build-clean-monitor
.\scripts\test.cmd build-clean-collector build-clean-monitor
python tests\review-monitor\verify_vendor.py
```

test_core는 공유 계산·상태 전이, test_windows는 실제 수집·결과 수명·자신이 생성한 자식 종료·핸들 루프, test_monitor는 합성 저장·관측창·Baseline·누출·rollback·schema·보관 회귀다. 검사 개수는 정확도·현실 시나리오 개수가 아니다. 정책 차단과 컴파일 오류를 구분해 보고하고 정책을 변경하지 않는다.

## 콘솔과 통합 headless

```powershell
.\build\resource_console.exe --self --count 3 --interval-ms 3000
.\build-monitor\monitor_gui.exe --headless --synthetic --db build-monitor\synthetic-smoke.sqlite --count 23 --interval-ms 100
.\build-monitor\monitor_gui.exe --headless --db build-monitor\local-smoke.sqlite --count 23
python tests\monitor_inspect.py build-monitor\local-smoke.sqlite
```

실제 DB에는 프로세스명·경로가 들어가므로 로컬에만 둔다. synthetic은 별도 DB와 표시를 사용하며 날짜 gate를 우회하지 않는다. 실제 23회는 약 1분의 동작 확인이며 여러 날짜 학습을 검증하지 않는다. 대기는 처리 후 시작한다.

화면 검증은 `monitor_gui.exe --db build-monitor\my-local-monitor.sqlite`로 열어 선택·스크롤, Pause/Resume, 오류·STALE, 피드백·취소·종료를 수동 확인한다. 실행 후 창을 사용자가 닫는다. headless 실행은 이 상호작용 검증을 대신하지 않는다.

## 기존 별도 검토 테스트

```powershell
.\tests\review-monitor\build-review.cmd build-clean-review
```

fixture가 기존 개인 DB를 덮지 않도록 새 작업 디렉터리에서 실행한다. executable 경로는 이동 전에 확정한다. fixture 경로는 실행 CWD의 build-review-monitor/다.

```powershell
$reviewExe = (Resolve-Path build-clean-review\test_independent.exe).Path
New-Item -ItemType Directory -Path build-clean-review\run\build-review-monitor -Force
Push-Location build-clean-review\run
& $reviewExe
& $reviewExe --minute-shadow --expect-ready
& $reviewExe --minute-selection
Pop-Location
```

이 코드는 과거 별도 reviewer가 작성한 테스트다. 동일 세션에서 재실행하는 결과는 이번 작업의 독립 에이전트 검토로 보고하지 않는다. allocation 주입은 engine과 storage에 적용한다. --live/--capacity/--load 및 inspect_sql.py/minute_shadow_sql.py는 목적별 선택 진단이다. minute_shadow_sql.py와 inspect_sql.py의 SQL은 **이전 결함 재현용**이며 현재 제품 알고리즘으로 복사하지 않는다.

## 성숙 Baseline 부하 도구

```powershell
.\build-monitor-benchmark.cmd build-clean-monitor\monitor.lib build-clean-benchmark
$benchExe = (Resolve-Path build-clean-benchmark\benchmark_monitor_batch.exe).Path
New-Item -ItemType Directory -Path build-clean-benchmark\run\build-monitor -Force
Push-Location build-clean-benchmark\run
& $benchExe --case all --phase after-cache --instances 500 --days 14
Pop-Location
```

이 도구는 14 UTC 날짜의 합성 성숙 history를 만들고 same/mixed/distinct path의 500개 동시 창 종료와 실제 self collector의 다음 QPC 간격을 측정한다. 합성 DB는 실행 CWD의 build-monitor/에 보존된다. --phase는 결과 구분용 label이며 오래된 구현을 복원하지 않는다. CPU·I/O long_gap 및 관측창 reset을 확인하며 여러 성능 테스트를 동시에 실행하지 않는다.

benchmark_monitor_query.exe는 본인 합성 fixture DB와 정확한 START_TICKS를 받아 기존/현재 조회 집합을 비교한다. 선택적으로 --cache-benchmark를 사용한다. 실제 개인 DB를 입력하지 않는다. build-mature-probe.cmd [REVIEW_OUT]는 먼저 만든 review objects에 연결되며 test_mature_probe.exe는 checkpoint가 끝난 본인의 합성 DB를 복사해 별도 실제 self 창 검증을 수행한다. 대형 도구는 기본 회귀의 필수 실행 항목이 아니다.

Public 업로드 전에는 git status, git diff --cached, git ls-files를 보고 DB·원시 기록·비밀값·산출물이 없는지 확인한다. 테스트 실패를 기대값 변경으로 숨기지 않는다.
