# GUI와 피드백 계약

Win32 화면과 worker 연결은 `gui/monitor_gui.c`에 있다. `monitor_gui.exe` 하나에 collector·storage·detection·engine을 정적으로 링크한다. C17, User32/Common Controls/GDI를 사용한다. 공개 snapshot과 명령 API는 [monitor.h](../../include/monitor.h)다.

worker가 `collect → monitor_ingest → snapshot publish → 기본 3초 대기`를 수행한다. 대기는 처리 후 시작하므로 정확한 3초 cadence를 보장하지 않는다. UI는 SQLite connection이나 수집기를 직접 호출하지 않는다. snapshot은 batch/engine과 독립된 복사이며 사용 완료 후 `monitor_snapshot_free`로 해제한다.

GUI 내부에서는 `GuiSnapshot`이 metrics·이름 표시 sidecar·관계 snapshot을 함께 소유한다. worker의 `publish_view`가 lock 아래 `latest` 슬롯으로 소유권을 넘기고 UI의 `consume_snapshot`이 `shown`으로 인수한다. `app.display` 및 행·이름·관계 포인터는 `shown` 수명 안에서만 빌린다. wrapper는 `gui_snapshot_free`로 한 번 해제하고 borrowed pointer를 별도로 해제하지 않는다. 피드백 명령은 접수 후 성공·실패·미실행 취소 중 하나로 종결한다. 성공 표시만 DB 저장 성공 뒤 적용한다. UI를 바꿀 때의 파일별 수정 위치는 [개발 안내](../development.md#기능별-수정-위치)를 따른다.

선택한 A안은 프로그램 목록과 오른쪽 상세다. 목록의 현재 사용량은 최신 원시 값, 개별 완료창과 상세 비교는 마지막 완료된 60~75초 창이다. 둘의 시각을 혼동하지 않는다. 상세는 경로·신원·오류, 관측 및 창 UTC, 실제 창 길이, 지표별 유효 관측/표본수, 과거 개수/날짜/분, 중앙값/MAD/상한/지속성을 보여 준다. 검색은 그룹 합계 범위를 바꾸지 않으며 정렬·표시 행과 원본 인스턴스 신원을 분리한다.

CPU는 %, RAM은 decimal GB(Working Set bytes / 1e9), 읽기·쓰기는 decimal MB/s(B/s / 1e6)로 표시하며 저장 단위는 바꾸지 않는다. 그룹 RAM은 공유 페이지가 중복될 수 있다. 결측 NA, 평소 학습 중, 비교 불가, 상한 이내(일부), 오래된 표본, 저장·수집 오류를 구분한다. 유효 0은 `0.00`, 양수이지만 표시 단위로 0.01 미만이면 `<0.01`, 결측은 `NA`다. 미확인·실패를 정상이나 안전으로 표시하지 않는다. 선택과 스크롤은 행 번호 대신 인스턴스 신원으로 보존한다. foreground는 순간 관측이며 사용자 필요성 표시에 사용하지 않는다.

Pause는 현재 회차 후 미완료 창과 지속성을 버린다. Resume은 새 수집기로 첫 표본부터 시작한다. 닫기는 stop event로 worker 정리를 기다리며 UI 메시지 처리를 유지한다. headless 모드도 같은 worker를 사용한다.

1시간 알림 억제, 24시간 예상 사용은 선택한 program의 기간 제한 피드백·알림 억제다. 후보 사실과 수치, 원자료, 학습 gate는 유지한다. 취소·만료 후 해제되며 같은 basename의 다른 경로에는 전파하지 않는다. 영구 정상 등록을 뜻하지 않는다. 저장 요청과 성공 응답을 분리하며 worker가 저장 성공을 확인한 뒤 화면에 적용한다.

접수 용량 16개는 대기 명령·worker가 가져간 명령·UI가 아직 소비하지 않은 결과를 합친 outstanding 수다. 접수 전에 결과 슬롯과 UI 소유 program 표시 노드를 확보한다. 포화는 busy, 할당 실패는 미접수로 알리며 DB 쓰기를 시작하지 않는다. 접수한 결과 저장에는 추가 할당이 없다. worker 초기화 실패·중간 명령 실패·종료에서는 실행하지 않은 명령을 취소하고, 이미 저장한 명령의 성공은 유지한다. UI의 마지막 drain은 worker join 뒤에도 수행한다.

응답을 소비해도 성공 표시를 바로 버리지 않는다. worker는 성공마다 단조 증가 revision을 주고 정상 snapshot에 포함 revision을 붙인다. UI의 program별 최신 성공은 정상 frame이 해당 revision을 포함하거나 정상 frame에서 program 부재가 확인될 때 정리한다. 행이 없는 오류 frame은 부재 관측이 아니며 정리 근거로 쓰지 않는다. 따라서 상태 수명은 마지막 정상 frame의 program과 접수 중인 명령 수에 제한된다. 실패/미실행 취소는 이전 성공을 덮지 않는다. 성공한 피드백 취소와 만료는 오래된 snapshot의 성공 표시가 되살아나지 않도록 유지한다. Pause·Resume의 session 전환만으로 program에 저장된 성공을 지우지 않으며 순서는 UTC 비교로 판정하지 않는다. 이 revision은 한 worker 안의 쓰기 순서이며 외부 writer를 통제하지 않는다.

SYNTHETIC은 별도 DB를 명시해야 하며 제목·상태에 표시한다. 합성 데모도 날짜 gate를 건너뛰지 않는다. 일반 실행의 기본 DB는 현재 작업 폴더의 `monitor-data.sqlite`이므로 실제 기록은 Git에 올리지 않는다.

별도의 `--ui-demo`는 엔진·수집·DB 없이 같은 GUI 표시 경로를 확인하는 고정 합성 snapshot이다. 실제 판정 결과로 해석하지 않으며 피드백 저장을 실행하지 않는다. 화면 크기·배율·조작과 검증 범위는 [A안 C 화면 안내](../native-ui.md), [현재 검증 기록](../validation.md)을 따른다. B안은 HTML 참고 자료로 보존한다.

수동 확인 항목은 선택 유지, 부분 결측·학습, 피드백·취소·만료, Pause/Resume, 종료, DPI·접근성·오래된 snapshot 표시다. headless 성공만으로 화면 상호작용이 검증됐다고 보고하지 않는다. AI 설명·역할·외부 보안 분석의 별도 표시 원칙은 [아키텍처](../architecture.md)의 확장 경계를 따른다.
