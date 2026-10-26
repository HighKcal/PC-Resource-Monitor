# GUI와 피드백 계약

Win32 화면과 worker 연결은 `gui/monitor_gui.c`에 있다. `monitor_gui.exe` 하나에 collector·storage·detection·engine을 정적으로 링크한다. C17, User32/Common Controls/GDI를 사용한다. 공개 snapshot과 명령 API는 [monitor.h](../../include/monitor.h)다.

worker가 `collect → monitor_ingest → snapshot publish → 기본 3초 대기`를 수행한다. 대기는 처리 후 시작하므로 정확한 3초 cadence를 보장하지 않는다. UI는 SQLite connection이나 수집기를 직접 호출하지 않는다. snapshot은 batch/engine과 독립된 복사이며 사용 완료 후 `monitor_snapshot_free`로 해제한다.

표의 now는 현재 원시 값, Last window와 상세 비교는 마지막 완료된 60~75초 창이다. 둘의 시각을 혼동하지 않는다. 상세는 경로·신원·오류, 관측 및 창 UTC, 실제 창 길이, metric별 coverage/표본수, 과거 개수/날짜/분, median/MAD/상한/지속성을 보여 준다.

결측 NA, learning, unavailable, normal(partial), STALE, 저장·수집 오류를 구분한다. 미확인·실패를 정상이나 안전으로 표시하지 않는다. 선택과 스크롤은 행 번호 대신 인스턴스 신원으로 보존한다. foreground는 순간 관측이며 사용자 필요성 표시에 사용하지 않는다.

Pause는 현재 회차 후 미완료 창과 지속성을 버린다. Resume은 새 수집기로 첫 표본부터 시작한다. 닫기는 stop event로 worker 정리를 기다리며 UI 메시지 처리를 유지한다. headless 모드도 같은 worker를 사용한다.

Ignore 1 hour, Expected 24 hours는 선택한 program의 기간 제한 피드백·알림 억제다. 후보 사실과 수치, 원자료, 학습 gate는 유지한다. 취소·만료 후 해제되며 같은 basename의 다른 경로에는 전파하지 않는다. 영구 정상 등록을 뜻하지 않는다.

SYNTHETIC은 별도 DB를 명시해야 하며 제목·상태에 표시한다. 합성 데모도 날짜 gate를 건너뛰지 않는다. 일반 실행의 기본 DB는 현재 작업 폴더의 `monitor-data.sqlite`이므로 실제 기록은 Git에 올리지 않는다.

수동 확인 항목은 선택 유지, 부분 결측·학습, 피드백·취소·만료, Pause/Resume, 종료, DPI·접근성·오래된 snapshot 표시다. headless 성공만으로 화면 상호작용이 검증됐다고 보고하지 않는다. AI 설명·역할·외부 보안 분석의 별도 표시 원칙은 [아키텍처](../architecture.md)의 확장 경계를 따른다.
