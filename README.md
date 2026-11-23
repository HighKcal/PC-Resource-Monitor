# PC-Resource-Monitor

Windows에서 실행 중인 프로세스가 내 PC에서 평소보다 CPU나 메모리를 많이 쓰는 상태가 이어지면 알림을 받을 수 있습니다. 알림을 누르면 어느 프로세스가 평소 상한을 얼마나 넘었는지, 지금은 얼마나 쓰고 있는지 확인할 수 있습니다.

**[Windows x64 다운로드 · PC-Resource-Monitor-win-x64.zip](https://github.com/HighKcal/PC-Resource-Monitor/releases/download/team-submission-2026-11-23/PC-Resource-Monitor-win-x64.zip)**

예를 들어 백그라운드 동기화 프로그램의 RAM 알림을 누르면, 해당 프로세스가 계속 실행 중일 때 상세 화면에서 이름과 RAM 기록을 볼 수 있습니다. **당시 구간 평균**이 1.2 GB이고 **당시 평소 상한**이 0.8 GB라면, 그 구간에는 상한보다 평균 0.4 GB 더 썼습니다. 같은 화면의 **현재 RAM**이 0.6 GB라면 마지막 측정 시각에는 이미 당시 상한 아래로 내려온 것입니다.

## 실행하기

1. 내려받은 ZIP의 **압축을 모두 풉니다**. 실행 파일과 DLL, 리소스가 같은 폴더 구조에 있어야 합니다.
2. 압축을 푼 곳에서 `win-x64/PcResourceMonitor.exe`를 실행합니다.
3. Windows 관리자 권한 확인 창에서 **예**를 선택하면 수집이 시작됩니다.

저장소 전체를 내려받았다면 [bin/win-x64](bin/win-x64/) 안의 `PcResourceMonitor.exe`를 실행하세요. 배포본에는 .NET 런타임이 포함되어 있습니다.

## 어떻게 동작하나요?

1. **실행 중인 프로세스의 RAM을 읽습니다.** C 수집기는 Windows의 `GetProcessMemoryInfo`로 프로세스의 메모리 중 현재 RAM에 올라와 있는 바이트 수(`WorkingSetSize`, 공유 페이지 포함)를 읽습니다. 프로세스 번호(PID)와 `GetProcessTimes`로 얻은 생성 시각을 함께 확인해, 나중에 같은 PID를 받은 프로세스와 구분합니다.
2. **표본을 약 1분씩 모읍니다.** C 엔진은 같은 프로세스의 표본을 60~75초 구간으로 묶습니다. 유효한 RAM 표본이 15개 이상이면 그 값들의 산술평균을 개인 기준과 비교합니다.
3. **이 PC의 과거 기록에서 평소 상한을 구합니다.** 로컬 SQLite에서 최근 14일(UTC 기준) 동안 같은 실행 파일 경로로 쌓인 학습용 RAM 구간 평균을 고릅니다. 과거 평균들의 중앙값에 평소 변동 폭을 고려한 여유를 더해 상한을 구합니다. 판정에는 서로 다른 분에 끝난 구간이 60개 이상, 기록이 있는 날이 3일 이상 필요합니다.
4. **연속 초과를 확인하고 알림을 요청합니다.** 기준이 준비된 뒤 같은 프로세스의 RAM 평균이 유효한 세 구간 연속으로 상한을 넘으면 알림 후보가 됩니다. 같은 실행 경로의 후보를 묶고, 사용자 설정과 반복 알림 제한을 적용해 보낼 항목을 고른 뒤 Windows에 트레이 알림을 요청합니다.
5. **클릭하면 당시와 지금을 함께 봅니다.** C# WPF 화면은 `MonitorBridge.dll`을 통해 C 엔진의 측정 결과와 알림 기록을 받습니다. 클릭 시 같은 수집 세션에서 같은 프로세스인지 다시 확인합니다. **당시 구간 평균**과 **당시 평소 상한**은 알림에 보관한 값이고, **현재 RAM**은 가장 최근 표본의 값입니다. 각 카드에 표시된 구간이나 측정 시각으로 두 시점을 확인할 수 있습니다.

## 사용량 읽기

- **그룹을 펼쳐 보세요.** ‘프로그램과 구성원’은 그룹별 현재 CPU·GPU·RAM 사용량을 보여 줍니다. 그룹을 펼쳐 구성원 하나를 선택하면 그 프로세스의 개인 기준과 측정값을 볼 수 있습니다. GPU는 현재 사용량을 보는 지표입니다. 평소 상한을 넘었는지 판단해 알림을 보내는 지표는 CPU, RAM, I/O 읽기·쓰기입니다.
- **첫날에는 ‘최근 기록과 비교’를 보세요.** 개인 기준을 학습하는 동안에도 같은 수집 세션에서 같은 프로세스의 변화를 볼 수 있습니다. 해당 지표에 직전 60분의 유효한 과거 구간이 5개 이상 있으면, 최근 기록의 중앙값(최대 30개 구간)과 가장 최근에 끝난 유효 구간의 평균을 나란히 표시합니다. 이 비교는 화면 설명용이며 알림은 위의 장기 기준을 따릅니다.

‘이 프로그램 알아보기’는 먼저 로컬 파일 정보와 서명을 확인합니다. 외부 자료 조회는 보낼 내용과 목적지를 확인해 동의한 뒤 시작하며, 번역은 별도로 동의한 공개 문장 한 개만 전송합니다.

## 궁금한 기능별로 처음 읽을 소스

| 궁금한 기능 | 처음 읽을 소스 | 확인할 수 있는 동작 |
| --- | --- | --- |
| 프로세스 수집 | [resource_collector.c](src/collector/resource_collector.c) | `read_process`가 생성 시각을 읽고 `read_memory`로 RAM 표본을 얻습니다. |
| 구간 평균 | [monitor_window.c](src/engine/monitor_window.c) | `monitor_window_observe`가 표본을 누적하고 `monitor_window_metric`이 RAM 표본 수와 평균을 확인합니다. |
| SQLite 저장과 이력 조회 | [monitor_storage.c](src/storage/monitor_storage.c) · [monitor_history.c](src/storage/monitor_history.c) | 원시 표본과 완료 구간을 저장합니다. `monitor_history_select_day`는 같은 실행 경로·지표의 학습용 과거 구간을 날짜별로 고릅니다. |
| 개인 기준 판정 | [baseline.c](src/detection/baseline.c) | `monitor_baseline`에서 과거 중앙값과 상한을 계산하고 연속 초과 횟수로 후보를 판정합니다. |
| 프로그램 그룹 합산 | [program_groups.c](src/grouping/program_groups.c) | `pg_groups_build`가 구성원을 그룹에 배정하고 각 구성원의 현재 표본에서 CPU·RAM·I/O를 합산합니다. |
| 알림 정책 | [notification_policy.c](src/notifications/notification_policy.c) | `notification_policy_observe`가 후보를 모으고 `notification_policy_dispatch`가 전달할 구성원과 지표를 다시 확인합니다. |
| C와 WPF 연결 | [Engine.cs](src/wpf/Engine.cs) · [bridge.c](src/wpf/native/bridge.c) | C#이 네이티브 DLL의 `bridge_collect`를 호출해 한 회차를 수집하고 `bridge_notification_click`으로 클릭한 알림 기록을 가져옵니다. |
| 알림 클릭과 상세 화면 | [WindowController.cs](src/wpf/WindowController.cs) · [MainWindow.xaml.cs](src/wpf/MainWindow.xaml.cs) | `Navigate`가 클릭한 프로세스를 선택하고 `RenderProcessAnalysis`가 당시 기록과 현재 표본을 각각의 카드에 표시합니다. |

## 소스에서 빌드하고 시험하기

Windows x64에서 Visual Studio 또는 Build Tools의 **C++를 사용한 데스크톱 개발**, Windows SDK, x64 .NET 10 SDK가 필요합니다. 첫 빌드에는 NuGet 연결 또는 런타임 팩이 들어 있는 패키지 캐시가 필요합니다. 저장소 루트에서 PowerShell로 실행하세요.

```powershell
.\build.ps1
.\test.ps1 -SkipBuild
```

첫 명령은 `bin/win-x64/`에 실행물을 만들고, 두 번째 명령은 방금 빌드한 제품으로 대표 회귀 19종을 실행합니다. 시험 결과는 `.test-results/`에 저장됩니다.
