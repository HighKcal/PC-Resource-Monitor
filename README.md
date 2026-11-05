# PC-Resource-Monitor

Windows C 자원 수집기, SQLite 로컬 이력, 개인화 Baseline 비교와 Win32 GUI를 연결한 팀 프로젝트다. 현재 사용량과 평소 패턴의 차이를 근거와 함께 보여 준다. 자원 사용만으로 악성·불필요 여부를 판정하거나 프로세스를 자동 종료하지 않는다.

이번 중간 제출은 `codex/change-event-prototype` 브랜치다. [원본 저장소](https://github.com/HighKcal/PC-Resource-Monitor)의 `main`으로 자동 병합하지 않는다. 후속 기여는 [Fork·PR 안내](CONTRIBUTING.md)를 따른다.

## 현재 구현 범위

- PID+생성시각으로 연결하는 한 회차 수집, 필드별 유효성·실패 진단, 실제 QPC 간격에 따른 CPU·I/O 계산.
- SQLite 원시 기록·완료 관측창·피드백 저장, strict-past 개인화 Baseline, 날짜별 대표 선택과 회차 캐시.
- A안 Win32 화면: 검색·정렬·상세, 현재 사용량과 마지막 완료창 비교, Pause/Resume, 기간 한정 피드백. 접수한 명령의 결과 전달과 성공 표시 수명을 구분한다.
- 프로그램 그룹의 최신 사용량 합계, 구성원 펼침과 세션 내 사용자 소속 지정. 자동 그룹은 부모 관계 기반 추정이며 그룹 Baseline은 만들지 않는다.
- 별도 변화 사건 prototype: capture·사건 처리·저장·평가·CSV export·full/retained replay. 보관 정리는 남아 있는 기록의 참조와 종료 근거를 보호한다.
- engine/store/window/source, 과거 조회/cache/Baseline 정책, prototype 책임별 컴파일 단위와 한국어 설명 주석. 독립 oracle과 실제 도달을 확인하는 실패주입 시험을 유지한다.

AI 설명·외부 악성코드 탐지 연동은 설계 단계다. 실제 외부 API 호출·파일 업로드·격리는 구현하지 않았다.

## 개발 환경과 빌드

Windows x64, Visual Studio 또는 Build Tools의 **Desktop development with C++**, Windows SDK, Git이 필요하다. 확장 시험 프로필과 vendor 해시 검사에는 Python 3가 필요하며 외부 Python 패키지는 사용하지 않는다. 기본 `.cmd` 빌드는 Python 없이 동작한다.

PowerShell에서 다음과 같이 받는다. 이미 checkout했다면 저장소 루트에서 빌드부터 실행한다.

```powershell
git clone --branch codex/change-event-prototype https://github.com/HighKcal/PC-Resource-Monitor.git
cd PC-Resource-Monitor
.\build.cmd
.\build-monitor.cmd
.\build-prototype.cmd build-prototype build-monitor
.\scripts\test.cmd
.\build-prototype\test_experiment.exe
python -X utf8 tests\review-monitor\verify_vendor.py
```

`build/`에 수집 콘솔·계산/Windows/표시 시험, `build-monitor/`에 통합 GUI·monitor/grouping 라이브러리와 시험, `build-prototype/`에 prototype CLI·GUI·시험이 생성된다. scripts는 `vswhere`로 x64 MSVC를 찾는다. 제품은 C17, `/W4 /WX /O2 /MT /utf-8`로 빌드하며 SQLite의 경고는 별도 취급한다.

SQLite 3.54.1 소스·헤더와 [출처·해시·배포 조건](third_party/sqlite/PROVENANCE.md)을 동봉하므로 checkout 후 빌드에 네트워크가 필요 없다. `storage/monitor_schema.h`는 실행에 필요한 생성 헤더이므로 `storage/schema.sql`과 함께 관리한다. 설치 프로그램이나 과거 실행물을 요구하지 않는다. 별도 출력 경로와 전체 22개 빌드 프로필은 [개발 안내](docs/development.md), 시험 범위는 [tests 안내](tests/README.md)에 있다.

## 실행과 화면 해석

```powershell
.\build\resource_console.exe --self --count 3
# 실제 로컬 수집·저장. 아직 없는 DB 이름을 골라 실행한다.
$gui = (Resolve-Path .\build-monitor\monitor_gui.exe).Path
$db = Join-Path $PWD 'build-monitor\my-local-monitor.sqlite'
Start-Process -FilePath $gui -ArgumentList ('--db "{0}"' -f $db) -WorkingDirectory $PWD.Path
# 수집·DB 없이 고정 합성 화면만 확인하려면:
Start-Process -FilePath $gui -ArgumentList '--ui-demo' -WorkingDirectory $PWD.Path
```

주 GUI는 `requireAdministrator` 매니페스트를 포함하므로 Windows가 UAC 승격을 요청한다. `--headless`, `--synthetic`, `--ui-demo`에도 같은 정책이 적용된다. 관리자 실행도 모든 프로세스의 정보 취득을 보장하지 않는다. 콘솔·시험·별도 prototype 실행물에는 이 승격 정책을 추가하지 않았다. 실제 DB에는 프로세스명과 경로가 들어가므로 로컬에만 보관한다.

- CPU는 **전체 논리 CPU 용량 대비 %**, RAM 원자료는 **WorkingSetSize bytes**, I/O는 **프로세스 읽기·쓰기 B/s**다. GUI는 RAM을 십진 GB, I/O를 MB/s로 표시한다.
- `NA`는 결측이며 유효한 0과 다르다. 작은 양수는 표시 정밀도에 따라 `<0.01`로 보인다. 실제 QPC 구간과 기록용 UTC는 별개다.
- 현재 수치와 마지막 완료창 비교는 시점이 다르다. 학습 부족·측정 불가·오래된 결과를 정상으로 바꾸지 않는다. 기본 3초는 처리 **후 대기**이므로 정확한 3초 관측 간격을 보장하지 않는다.
- 그룹 `*partial X/Y`는 유효한 X개 구성원의 부분 합계다. Working Set 합은 공유 페이지를 중복 계산할 수 있다. 사용자 지정은 세션에만 유지되며 Resume·재시작·PID 재사용에 승계하지 않는다. 피드백은 개별 프로세스에서 사용한다.

자세한 조작은 [실제 C 화면](docs/native-ui.md), [그룹 계약](docs/contracts/process-grouping.md)을 참고한다. [HTML A/B 시안](ux/monitor-prototype/README.md)은 합성 디자인 참고 자료이며 실제 수집 화면이나 판정 결과가 아니다.

## 변화 사건 prototype

다음 demo는 **3 UTC 날짜의 합성 입력**을 실제 집계·Baseline 경로로 처리한다. 두 DB는 존재하지 않는 새 이름으로 지정한다.

```powershell
.\build-prototype\prototype_cli.exe demo --source build-prototype\demo-01-monitor.sqlite --db build-prototype\demo-01-events.sqlite
.\build-prototype\prototype_gui.exe --db build-prototype\demo-01-events.sqlite
```

사건 평가를 별도 원장에 저장하며 Baseline 학습이나 알림 억제에 반영하지 않는다. 실제 수집·조회·평가·export·replay 명령과 28일 평가 **계획**은 [prototype 안내](docs/prototype-guide.md)에 있다. 합성 시험은 실제 장기 학습의 정확도나 사용자 효용을 증명하지 않는다.

## 구조와 읽는 순서

| 경로 | 책임 |
|---|---|
| `collector/`, `include/resource_*.h` | Windows 수집, 순수 계산, 단위·유효성·표시 진단 |
| `engine/` | ingest 순서, 인스턴스·관측창, 실패 복구, commit 뒤 결과 공개 |
| `storage/` | opaque store의 DB·statement, 값 저장, 날짜별 과거 조회·source 읽기 |
| `detection/` | 회차 history/cache와 통계·학습·판정 정책 |
| `prototype/` | coordinator, capture, store, reducer, history/export, replay, 독립 retention |
| `grouping/`, `gui/` | 관계·세션 소속·합계, worker/UI 전달과 화면 |
| `app/`, `scripts/`, `tests/` | CLI, 공통 컴파일 목록, 회귀·실패주입·합성 부하 도구 |

[팀 읽기 순서](docs/team-guide.md) → [아키텍처·소유권](docs/architecture.md) → [개발 명령](docs/development.md) → [검증 결과와 한계](docs/validation.md)를 참고한다. 내부 경로를 직접 사용하던 통합자는 [이행 안내](docs/migration.md)를 확인한다. 공개 API/ABI·기존 DB 형식과 수집·판정 의미는 유지했다.

## 검증 상태와 공개 범위

이번 제출 준비에서 실행한 깨끗한 빌드와 과거 기능·실패·성능 검증을 [validation](docs/validation.md)에 구분해 기록한다. 성숙한 고유 경로 다수의 완료 회차는 3초를 넘는 비용이 남아 있다. 실제 전경 관리자 GUI 전체 조작, 외부 UI 조회가 없는 최종 GUI 성능, 장기 효용·누수·모든 동시 쓰기 실패는 완료로 주장하지 않는다.

실사용 DB·WAL/SHM, 개인 프로세스 목록·측정 로그·화면 캡처, 자격증명, 빌드 산출물, 과거 보존본과 로컬 작업 로그는 업로드하지 않는다. 필요한 합성 fixture·ABI 기대값·SQLite·실행 스키마는 포함한다. 프로젝트 자체의 배포 라이선스는 팀 결정이 필요하며 SQLite의 public-domain 조건은 별도다.
