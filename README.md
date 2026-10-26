# PC-Resource-Monitor

Windows C 자원 수집기와 SQLite 로컬 기록, 개인화 Baseline 비교, Win32 GUI를 통합한 팀 프로젝트다. 장기 개인 패턴과 현재 관측창을 비교해 **자원 이상 후보**를 보여 준다. 악성·불필요 판정과 자동 프로세스 종료는 수행하지 않는다.

원본 저장소: [HighKcal/PC-Resource-Monitor](https://github.com/HighKcal/PC-Resource-Monitor). 협업은 **원본 → 개인 Fork → 기능 브랜치 → 원본 main으로 PR → 검토·병합** 순서다. [영역별 필독 문서](docs/team-guide.md)와 [CONTRIBUTING](CONTRIBUTING.md)부터 읽는다.

## 구현과 책임

| 영역 | 현재 책임 |
|---|---|
| collector | Windows API, PID+생성시각, 자원 단위와 필드 유효성, 경로 신원 어댑터 |
| storage | SQLite, 스키마, 저장·조회, 보관, 피드백 원장 |
| detection | 개인화 Baseline, 통계·상한·지속성·이상 후보 정책 |
| gui | 목록·상세·상태 표시, 선택·Pause/Resume·사용자 피드백 |

위 표는 영역별 유지보수 책임을 설명한다. 공통 engine·헤더·빌드·테스트는 영향받는 영역의 담당자가 함께 검토한다. GitHub 사용자명을 추정한 CODEOWNERS는 두지 않았다.

```text
collector/              Windows 수집·계산·monitor identity adapter
storage/                SQLite backend·내부 helper·실행 스키마·schema.sql
detection/              Baseline 통계·학습 gate·이상 후보 판정
gui/                    Win32 GUI 및 worker 실행 흐름
engine/                 batch 트랜잭션·관측창·snapshot 통합, 내부 상태
include/                공개 C 자료형과 API (기존 계약 유지)
app/                    수집기 콘솔 실행 예제
tests/collector/        계산·실제 Windows 수집 테스트
tests/integration/      저장·관측창·Baseline·실패·회귀 테스트
tests/performance/      성숙 Baseline 합성 부하 도구
tests/review-monitor/   기존 별도 검토 테스트와 진단 도구
third_party/sqlite/     고정 SQLite 3.54.1 소스·헤더·출처
scripts/                MSVC 탐색·빌드·테스트
docs/                   팀 안내·아키텍처·계약·검증
.github/                PR 양식
```

## 빠른 빌드와 실행

Windows x64, Visual Studio의 **Desktop development with C++**, Windows SDK가 필요하다. PowerShell에서 저장소 루트 기준으로 실행한다. scripts는 vswhere로 x64 MSVC를 찾는다. C17, 제품 `/W4 /WX`, `/O2 /MT`, UTF-8 소스이며 SQLite만 vendor 경고를 격리한다. 빌드·실행에 인터넷은 필요하지 않다.

```powershell
.\build.cmd
.\build-monitor.cmd
.\scripts\test.cmd
.\build\resource_console.exe --self --count 3
.\build-monitor\monitor_gui.exe --db build-monitor\my-local-monitor.sqlite
```

기존 루트 build 명령은 새 scripts의 진입점으로 유지했다. 수집기 static library와 콘솔, 하나의 통합 `monitor_gui.exe`, 테스트가 생성된다. 별도 출력 폴더를 지정한 클린 빌드와 확장 검증은 [development](docs/development.md)에 있다. SQLite는 [출처·해시·배포 조건](third_party/sqlite/PROVENANCE.md)을 유지한다.

짧은 합성 통합 실행은 다음과 같다. 합성도 실제 여러 날짜 학습을 대신하지 않으며 별도 DB를 사용한다.

```powershell
.\build-monitor\monitor_gui.exe --headless --synthetic --db build-monitor\synthetic-demo.sqlite --count 23 --interval-ms 100
```

## 데이터 흐름과 인터페이스

```text
Windows API → collector → ResourceBatch → engine
                                      ↘ storage: raw/완료창 transaction
                                       ↘ detection: strictly past Baseline
                                        → 독립 MonitorSnapshot → GUI
GUI 피드백 → worker → storage feedback (학습값은 유지)
```

[resource_collector.h](include/resource_collector.h)의 collect는 한 회차만 수집한다. batch 배열·문자열은 batch가 소유하고 `resource_batch_free`로 해제한다. CPU는 전체 논리 CPU 용량 대비 %, RAM은 WorkingSetSize bytes, I/O는 프로세스 read/write B/s다. 실제 QPC 간격과 기록용 UTC를 분리하고, PID+생성시각으로 인스턴스를 연결한다. 결측은 0으로 해석하지 않는다.

[monitor.h](include/monitor.h)는 worker 전용 engine API와 snapshot 소유권을 정의한다. engine은 한 batch의 원자적 저장과 60~75초 관측창을 연결하고, detection은 현재 창 이전의 과거만 비교한다. 최신 종료 분 대표 선택·날짜 cap·회차 cutoff와 연결별 16MiB SQLite 페이지 cache 설정을 보존했다. GUI는 복사된 snapshot만 소비하며 now와 마지막 완료창을 구분한다. [아키텍처와 의존 관계](docs/architecture.md), [담당별 계약](docs/team-guide.md)에 상세 규격이 있다.

## 검증과 한계

이번 구조 정리의 클린 빌드·전후 회귀·공개 점검 결과는 [validation](docs/validation.md)에 있다. 공개 API와 DB 스키마는 유지했으며 C 파일 위치와 내부 연결 이름은 변경했다. 수동 소스 목록을 사용하는 통합자는 [migration](docs/migration.md)을 확인한다.

정확한 3초 수집 cadence, 모든 PC에서의 성능 상한, 실제 여러 날 학습의 오탐·미탐 정확도는 보장하지 않는다. 기본 3초는 처리 후 대기다. 경로 문자열별 개별 인스턴스 분포이며 앱 전체 합계·파일 버전·사용자 필요성은 확인하지 않는다. GUI의 DPI·접근성·장시간 사용과 과거 OS 호환성은 추가 검증 대상이다.

DB와 WAL/SHM, 프로세스 목록·개인 경로, 화면 캡처·측정 로그, 빌드 산출물과 자격증명은 공개하지 않는다. 로컬 과거 연구/작업 로그는 보존하고 공개용 계약·검증 문서를 별도로 정리했다. 프로젝트 소스의 배포 라이선스는 팀이 결정해야 한다. SQLite의 public-domain 조건은 별도로 명시한다.

AI 설명, 역할 근거, 별도 악성코드 탐지 프로그램 연동은 **설계 단계의 선택 기능**이다. 실제 API 호출·파일 업로드·탐지 엔진 복제·자동 종료/격리는 구현하지 않았다. 자원 후보와 보안 결과를 하나의 안전/불필요 점수로 합치지 않는다. 향후 비동기 식별·실패·만료 계약은 [확장 경계](docs/architecture.md#선택적-확장-경계)를 따른다.
