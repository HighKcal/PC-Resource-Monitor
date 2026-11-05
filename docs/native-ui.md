# A안 Win32 화면

프로그램 목록에서 현재 사용량을 살펴보고, 그룹을 펼쳐 구성원 기여를 확인한 다음 오른쪽에서 선택한 프로세스의 과거 비교 근거를 읽는 구조다. HTML A안이 선택된 기준이며 B안은 `ux/monitor-prototype/dist/`에 참고용으로 보존한다. 제품은 C17/Win32이며 웹 런타임을 포함하지 않는다.

## 실행

제품 `monitor_gui.exe`는 매니페스트의 `requireAdministrator`, `uiAccess=false`로 관리자 실행을 요청한다. 탐색기에서 실행하거나 아래 `Start-Process` 명령을 사용하면 Windows UAC가 필요한 승인을 요청한다. 관리자 계정으로 로그인한 상태와 실행 중인 프로세스의 상승 토큰은 다르다. 승인을 취소하면 프로그램은 시작하지 않으며, 이미 상승된 셸에서 시작하면 별도의 UAC 창 없이 해당 권한을 이어받을 수 있다. 이 정책은 같은 실행 파일의 `--ui-demo`, `--headless`, `--synthetic`에도 적용된다.

저장소 루트의 PowerShell에서 실행한다. DB는 아직 없는 로컬 경로를 선택한다.

```powershell
.\build-monitor.cmd build-native-ui
$gui = (Resolve-Path .\build-native-ui\monitor_gui.exe).Path
Start-Process -FilePath $gui -ArgumentList '--ui-demo' -WorkingDirectory $PWD.Path
$db = Join-Path $PWD 'build-native-ui\my-monitor.sqlite'
Start-Process -FilePath $gui -ArgumentList ('--db "{0}"' -f $db) -WorkingDirectory $PWD.Path
```

`--ui-demo`는 표시 경로만 검수하는 합성 snapshot이다. 비교 수치도 예시이며 실제 엔진을 통과한 결과가 아니다. 기존 `--synthetic --db <별도경로>`는 수집 입력을 합성하는 통합 검수로 실제 저장·집계를 수행한다. 두 모드는 목적이 다르다.

관리자 실행은 접근 가능한 프로세스 정보의 범위를 넓힐 수 있지만 모든 이름·경로·수치를 보장하지 않는다. 제품은 `SeDebugPrivilege`를 활성화하지 않으며 기존 최소 조회 권한과 필드별 유효성·실패 이유를 유지한다. 별도의 수집 콘솔과 시험 실행 파일에는 이 GUI의 관리자 요청 매니페스트를 추가하지 않으며, 기존과 같이 실행한 부모 프로세스의 권한을 따른다.

재빌드 시 `gui/monitor_gui.manifest`가 제품 GUI에 포함된다. `scripts/build-monitor.cmd`와 그룹 변경용 `scripts/build-grouping.cmd` 모두 같은 정책을 적용하며 기존 Common-Controls v6 의존성과 코드에서 설정하는 DPI 처리를 유지한다. 링크 후 내장 매니페스트를 `monitor_gui.exe.manifest`에도 추출하여 이전 빌드의 외부 매니페스트가 남지 않게 한다. 직접 링크할 때도 이 빌드 진입점을 사용한다. Microsoft의 [UAC 매니페스트 옵션](https://learn.microsoft.com/en-us/cpp/build/reference/manifestuac-embeds-uac-information-in-manifest?view=msvc-170)과 [매니페스트 입력 병합](https://learn.microsoft.com/en-us/cpp/build/reference/manifestinput-specify-manifest-input?view=msvc-170)을 따른다.

## 읽는 순서와 단위

| 화면 | 답하는 질문 | 의미와 한계 |
|---|---|---|
| 현재 사용량 목록 | 지금 무엇이 자원을 많이 쓰는가? | 가장 최근 표본. CPU와 I/O는 실제 관측 구간의 비율이다. |
| 펼친 구성원 | 누가 그룹 합계에 기여했는가? | 현재 소속에 따라 각 인스턴스는 한 번만 포함한다. 일부 값이 빠지면 부분 합계다. |
| 오른쪽 현재 값 | 선택한 항목의 수치와 수집 상태는? | 최신 표본 시각, 지표별 결측 이유와 프로세스 신원을 확인한다. |
| 오른쪽 완료창 비교 | 평소와 무엇이 다르고 근거는 충분한가? | 완료된 관측창의 수치와 그 이전 이력만 비교한다. 현재 값과 같은 시점이 아니다. |

- **CPU %**: 전체 논리 CPU 용량 대비 비율.
- **RAM GB**: Working Set bytes / 1,000,000,000. 전체 PC 메모리 대비 사용률이 아니며 공유 페이지를 중복 합산할 수 있다. 수집·DB의 bytes 계약은 그대로다.
- **읽기·쓰기 MB/s**: 프로세스 I/O B/s / 1,000,000. 실제 디스크 사용률이 아니다.
- **평소 기준(Baseline)**: 같은 실행 경로의 과거 완료창에서 얻은 개인화 비교 기준. 학습 부족과 측정 불가를 상한 이내로 바꾸지 않는다.
- **확인할 변화**: 자원 사용의 이상 후보다. 악성·불필요 또는 안전 판정이 아니다.

이름에 `[목록 이름]`과 PID가 붙으면 Windows 목록에서 확보한 참고 이름이다. 상세에서 핸들 이름·목록 이름·이름 확인 불가를 구분한다. 참고 이름만 복구되어도 CPU/RAM/I/O는 여전히 NA일 수 있다. 접근 권한 부족, 개별 API 실패, 첫 표본 대기, 신원 확인 불가, 비교 자료 부족을 각 필드와 상세 진단에서 구분한다. 목록 이름은 저장/Baseline/그룹 소속 연결에 사용하지 않는다. [수집 계약](contracts/collector.md)을 참고한다.

그룹에는 현재 합계만 있으며 그룹 Baseline을 만들지 않는다. 개별 구성원을 선택해야 과거 비교와 피드백을 사용할 수 있다. 현재 표본 결측 때문에 비교 상태가 중단된 경우 남아 있는 지난 완료창 수치는 참고 근거로만 표시한다.

## 보존하는 동작

수집·저장·Baseline·그룹 모델과 공개 헤더는 변경하지 않는다. UI는 worker가 넘긴 복사 snapshot을 표시한다. 피드백은 선택한 실행 경로의 기간 한정 알림 억제이며 학습이나 임계값을 고치지 않는다. 저장 성공 응답 전에 적용되었다고 표시하지 않는다.

자동 그룹은 부모 관계 기반 추정이다. 사용자 지정은 사용자가 정한 소속이며 세션에만 유지한다. Pause 동안 화면을 유지하고 Resume 때 수집기를 새로 시작하면서 선택·펼침·그룹 지정을 초기화하는 기존 계약을 유지한다.

변화 사건 기록·독립 평가·내보내기·재생은 별도 `prototype_gui.exe`의 기능이다. 이번 기본 GUI 개편에 통합하지 않았다.

## HTML과 실제 화면

목록과 오른쪽 상세라는 정보 구조를 유지하되 Windows ListView, EDIT, COMBOBOX, 버튼을 사용한다. 제한된 CustomDraw가 그룹 색과 행 배경을 담당한다. 이름 열은 사용자가 조정하고 긴 이름은 목록에서 줄이되 상세에서는 원문을 읽을 수 있게 한다. 숫자는 오른쪽 정렬한다.

배율은 Windows DPI를 따라 글꼴·간격·열·행 높이를 조정한다. `--ui-scale 100|125|150|200`은 검수용 레이아웃 모사이며 실제 OS 배율 전환 시험을 대신하지 않는다. 최소 영역과 키보드·실행 결과의 최종 확인은 [검증 범위](validation.md)에 연결한다.

기본 client 영역은 1320×820 DIP, 최소는 960×640 DIP다. 오른쪽 패널은 344~460 DIP를 확보하고 남는 폭을 목록에 쓴다. 본문은 맑은 고딕 11pt, 행은 36 DIP다. 이름 열을 직접 늘리면 그 폭을 같은 실행의 resize/DPI 변경에도 유지한다. 좁은 화면에서 이름을 줄여 표시하고 목록의 가로 스크롤과 상세의 세로 스크롤을 사용한다. 큰 합성 수치 1234.56 GB와 12345.67 MB/s를 검수 입력에 포함했다.

Tab으로 컨트롤을 이동하고, 목록의 위·아래 키로 선택한다. 그룹 행의 오른쪽 키는 펼침, 왼쪽 키는 접기다. 버튼은 Space, 정렬·지표 콤보는 방향키로 조작할 수 있다. 열 제목을 클릭하면 해당 이름/현재 지표의 정렬 방향이 전환된다. 검색에 가려진 항목은 상세에서 잘못 다른 항목으로 선택하지 않는다.

```powershell
# 실제 Windows 설정을 바꾸지 않는 최소 창 / 배율 모사
Start-Process -FilePath $gui -ArgumentList '--ui-demo --ui-size 960x640 --ui-scale 150' -WorkingDirectory $PWD.Path
```

## 검증 상태

숨김 컨트롤·명령 fixture와 실제 worker/합성 SQLite 회귀는 [검증 요약](validation.md)에 구분한다. 실제 전경 관리자 GUI 전체 조작과 외부 UI 조회가 제거된 최종 GUI 성능은 완료로 주장하지 않는다. HTML 시안, 합성 표시, 실제 저장, 실제 화면 전체 검증은 서로 다른 범위다. 이번 제출 준비에서는 새 checkout 빌드·필요한 회귀만 수행하며 UAC·화면 자동화·성능 측정을 반복하지 않는다.
