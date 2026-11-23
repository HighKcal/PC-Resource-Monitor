# PC-Resource-Monitor

Windows PC의 프로세스별 자원 사용량을 로컬에 기록하고, 현재 사용량을 평소 패턴과 비교해 보여 주는 팀 프로젝트입니다.

## 실행 · Windows x64

1. [최종 릴리즈의 Windows x64 ZIP](https://github.com/HighKcal/PC-Resource-Monitor/releases/download/team-submission-2026-11-23/PC-Resource-Monitor-win-x64.zip)을 내려받아 전체를 풉니다.
2. `win-x64/PcResourceMonitor.exe`를 열고 Windows 관리자 승인 창에서 **예**를 선택합니다. DLL과 리소스는 실행 파일과 함께 둡니다.

저장소를 내려받았다면 [bin/win-x64](bin/win-x64/) 폴더 전체가 필요합니다. 저장소 루트에서 PowerShell로 실행할 수 있습니다.

```powershell
.\bin\win-x64\PcResourceMonitor.exe
```

관리자 승인 후 수집한 이력은 현재 사용자의 `%LOCALAPPDATA%\PC-Resource-Monitor\Wpf\history.sqlite`에 저장합니다. 배포본에는 .NET 런타임이 포함되어 있습니다. ZIP의 SHA-256은 [릴리즈 페이지](https://github.com/HighKcal/PC-Resource-Monitor/releases/tag/team-submission-2026-11-23)에서 확인할 수 있습니다.

## 소스·빌드·시험

[src/](src/)에 WPF 화면과 C 엔진의 소스가 있습니다. Windows x64에서 빌드하려면 Visual Studio 또는 Build Tools의 **C++를 사용한 데스크톱 개발**, Windows SDK, x64 .NET 10 SDK가 필요합니다. `dotnet --list-sdks`로 SDK를 확인할 수 있습니다. 첫 빌드에는 NuGet 연결 또는 런타임 팩이 들어 있는 패키지 캐시가 필요합니다.

저장소 루트에서 PowerShell로 빌드하고, 방금 빌드한 제품으로 대표 회귀 19종을 실행합니다.

```powershell
.\build.ps1
.\test.ps1 -SkipBuild
```

`test.ps1`을 단독으로 실행하면 제품 빌드부터 진행합니다. 중간 빌드는 `.build/`, 회귀 결과는 `.test-results/`에 저장됩니다. C 최근 비교 시험은 Windows 임시 폴더에 시험 DB를 만들고 종료할 때 지웁니다.

## 규칙을 바꿀 때 처음 읽을 곳

| 변경하려는 내용 | 시작점과 다음 호출 |
|---|---|
| 수집 간격·자원값·화면 전달 | [MonitorLoop.Run](src/wpf/MonitorLoop.cs)에서 수집과 대기를 찾는다. [resource_collect_with_private_ws](src/collector/resource_collector.c)와 [rc_core_update](src/collector/resource_core.c)에서 PID·생성시각, 필드별 결측, QPC 차분을 읽는다. [WindowController.Consume](src/wpf/WindowController.cs)가 UI 스레드에서 프레임을 적용한다. |
| 회차·피드백 저장과 재시도 | [monitor_ingest_with_commit_ref와 monitor_feedback](src/engine/monitor.c)에서 성공 반환과 스냅숏 공개 시점을 본다. [monitor_store_commit_feedback와 monitor_store_trim](src/storage/monitor_storage.c)에서 각각의 트랜잭션과 롤백을 찾는다. |
| 첫날 비교·개인 상한 | [monitor_policy.h](src/detection/monitor_policy.h)에서 필요한 기록 수를 찾는다. [read_recent_history](src/storage/monitor_source.c)가 최신 유효 완료창과 별도로 과거 창을 고른다. [monitor_baseline](src/detection/baseline.c)이 장기 기준과 지표별 연속 초과를 판단한다. |
| 표시 그룹·알림 클릭 | [pg_groups_build](src/grouping/program_groups.c)가 화면의 구성원과 현재 합계를 만든다. [notification_policy_observe](src/notifications/notification_policy.c)에서 프로그램별 알림 요청으로 이어진다. [MonitorLoop.Pump와 Tray](src/wpf/MonitorLoop.cs), [SelectAnalysisEvidence](src/wpf/MainWindow.xaml.cs)에서 클릭한 구성원·지표의 당시 근거와 별도 현재 표본을 본다. |
| 프로그램 알아보기 | [ProgramInfoWindow.RefreshAsync와 ExecuteAsync](src/wpf/ProgramInfoWindow.xaml.cs)에서 [pc_program_execute](src/wpf/native/companion.c)로 이어진다. [program_info_collect](src/gui/program_info.h)는 로컬 근거, [program_research_lookup](src/gui/program_research_lookup.h)은 동의 후 공개 자료 조회를 맡는다. |

화면을 처음부터 따라가려면 [App.Main](src/wpf/App.cs)과 [MainWindow.xaml](src/wpf/MainWindow.xaml)을 연다. 저장 스키마를 편집할 때는 [SQL 원문](src/storage/schema.sql)과 실제 C 빌드에 들어가는 [문자열 헤더](src/storage/monitor_schema.h)를 함께 확인한다. SQLite의 출처는 [동봉 기록](src/third_party/sqlite/PROVENANCE.md), 글꼴 라이선스는 [Assets](src/wpf/Assets), .NET 고지는 [licenses](licenses)에 있다.
