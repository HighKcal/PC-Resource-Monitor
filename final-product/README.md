# 내 PC 자원 모니터 · Windows x64

**이후 제품 수정의 출발점은 이 폴더 한 곳이다.** `src/`에는 팀이 편집하는 WPF 화면, C 엔진, 공용 헤더, 리소스와 회귀 입력이 있다. `bin/win-x64/`에는 이 소스로 빌드한 실행 파일과 함께 배포해야 하는 DLL이 있다. 이 폴더 전체를 다른 위치로 옮겨도 빌드할 수 있다.

## 실행

PowerShell에서 다음 명령으로 정식 제품을 연다. Windows 권한 확인 후 실수집을 시작하며, 이력은 현재 사용자의 `%LOCALAPPDATA%\PC-Resource-Monitor\Wpf\history.sqlite`에 저장한다.

```powershell
& 'C:\collect-data-window-teampj\final-product\bin\win-x64\PcResourceMonitor.exe'
```

## 빌드와 회귀

Windows x64에서 Visual Studio 또는 Build Tools의 **C++를 사용한 데스크톱 개발**과 Windows SDK, x64 .NET 10 SDK, PowerShell이 필요하다. `dotnet --list-sdks`로 SDK를 확인한다. 첫 빌드와 회귀 실행에는 .NET 런타임 팩을 복원할 NuGet 연결 또는 미리 채운 패키지 캐시가 필요하다. 배포된 실행 파일은 .NET 런타임을 포함한다.

```powershell
$product = 'C:\collect-data-window-teampj\final-product'
& "$product\build.ps1"
& "$product\test.ps1"
```

다른 경로에서는 `$product`만 바꾼다. `test.ps1`은 기본적으로 제품도 다시 빌드한다. 방금 같은 소스로 빌드했다면 `& "$product\test.ps1" -SkipBuild`로 회귀만 실행할 수 있다. 중간 빌드는 `.build/`, 회귀 결과는 `.test-results/`에 생긴다. C 최근 비교 시험은 Windows 임시 폴더에 시험 DB를 만들고 종료할 때 지운다.

## 규칙을 바꿀 때 처음 읽을 곳

| 변경하려는 내용 | 시작점과 다음 호출 |
|---|---|
| 수집 간격·자원값·화면 전달 | [MonitorLoop.Run](src/wpf/MonitorLoop.cs)에서 수집과 대기를 찾는다. [resource_collect_with_private_ws](src/collector/resource_collector.c)와 [rc_core_update](src/collector/resource_core.c)에서 PID·생성시각, 필드별 결측, QPC 차분을 읽는다. [WindowController.Consume](src/wpf/WindowController.cs)가 UI 스레드에서 프레임을 적용한다. |
| 회차·피드백 저장과 재시도 | [monitor_ingest_with_commit_ref와 monitor_feedback](src/engine/monitor.c)에서 성공 반환과 스냅숏 공개 시점을 본다. [monitor_store_commit_feedback와 monitor_store_trim](src/storage/monitor_storage.c)에서 각각의 트랜잭션과 롤백을 찾는다. |
| 첫날 비교·개인 상한 | [monitor_policy.h](src/detection/monitor_policy.h)에서 필요한 기록 수를 찾는다. [read_recent_history](src/storage/monitor_source.c)가 최신 유효 완료창과 별도로 과거 창을 고른다. [monitor_baseline](src/detection/baseline.c)이 장기 기준과 지표별 연속 초과를 판단한다. |
| 표시 그룹·알림 클릭 | [pg_groups_build](src/grouping/program_groups.c)가 화면의 구성원과 현재 합계를 만든다. [notification_policy_observe](src/notifications/notification_policy.c)에서 프로그램별 알림 요청으로 이어진다. [MonitorLoop.Pump와 Tray](src/wpf/MonitorLoop.cs), [SelectAnalysisEvidence](src/wpf/MainWindow.xaml.cs)에서 클릭한 구성원·지표의 당시 근거와 별도 현재 표본을 본다. |
| 프로그램 알아보기 | [ProgramInfoWindow.RefreshAsync와 ExecuteAsync](src/wpf/ProgramInfoWindow.xaml.cs)에서 [pc_program_execute](src/wpf/native/companion.c)로 이어진다. [program_info_collect](src/gui/program_info.h)는 로컬 근거, [program_research_lookup](src/gui/program_research_lookup.h)은 동의 후 공개 자료 조회를 맡는다. |

화면을 처음부터 따라가려면 [App.Main](src/wpf/App.cs)과 [MainWindow.xaml](src/wpf/MainWindow.xaml)을 연다. 저장 스키마를 편집할 때는 [SQL 원문](src/storage/schema.sql)과 실제 C 빌드에 들어가는 [문자열 헤더](src/storage/monitor_schema.h)를 함께 확인한다. SQLite의 출처는 [동봉 기록](src/third_party/sqlite/PROVENANCE.md), 글꼴 라이선스는 [Assets](src/wpf/Assets), .NET 고지는 [licenses](licenses)에 있다.
