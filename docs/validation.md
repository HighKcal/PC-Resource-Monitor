# 구조 정리 검증 — 2026-10-26

이번 작업은 단일 세션이 조사·분리·빌드·테스트·자체 검토를 수행했다. 아래 기존 별도 검토 테스트의 재실행을 새로운 독립 에이전트 검토로 표현하지 않는다. 실행 환경은 Windows 11 x64, MSVC C17, 제품 `/W4 /WX /O2 /MT`다.

## 전후 기능 회귀

기존 산출물과 별도인 비어 있는 출력 디렉터리에서 각각 컴파일했다. 최신 변경 전 monitor.c SHA256은 `e6e9e1943936b6809da7e8ad3dab47d01b84ead1b6c12d8434a218c3faa3f630`이며, 종료 분 대표 선택과 16MiB SQLite cache가 적용된 버전을 기준으로 했다.

| 항목 | 이동 전 | 이동 후 |
|---|---|---|
| collector/console/테스트 빌드 | 성공 | 성공 |
| 통합 monitor/GUI/테스트 빌드 | 성공 | 성공 |
| test_core | 65 checks 통과 | 65 checks 통과 |
| test_windows | 843 checks 통과 | 843 checks 통과 |
| test_monitor | 9,160 checks / 0 failures | 9,160 checks / 0 failures |

추가로 새 소스 목록에서 기존 별도 검토 테스트를 다시 빌드했다. 기본 9,058, minute-shadow 2,151, minute-selection 38,870 checks 모두 실패 0이었다. allocation 주입을 storage로 이동한 calloc에도 적용해 기존 OOM/rollback 검증을 유지했다. 성숙 부하 두 도구와 mature-probe 도구의 빌드도 성공했다. mature-probe의 긴 실제 완료창 실행은 이번에는 수행하지 않았다.

공개 헤더·수집/identity/GUI/콘솔 및 기본 테스트·부하 소스 등 이동만 한 파일은 원본과 바이트 비교했다. monitor.c 34개 함수는 내부 helper 식별자·static 연결 범위 외에 본문을 보존했다. schema.sql과 실행 헤더의 SQL 문자열은 원본 및 서로 간 일치를 확인했으며 주석의 출처 경로만 갱신했다.

## 성숙 Baseline 500개 동시 창 종료

`benchmark_monitor_batch --case all --phase after-cache --instances 500 --days 14`를 새 library로 실행했다. 14개 UTC 날짜의 합성 history에서 각 500개 창을 완료하고 896표본·14날짜·896종료분 및 CPU attention/streak 1, RAM 20표본을 확인했다. 총 4,204,121 assertions / 0 failures였다.

| 경로 분포 | 전체 ingest | 실제 self 다음 QPC 관측 간격 |
|---|---|---|
| 같은 경로 1개 | 5,069.907 ms | 8.101804 s |
| 혼합 경로 50개 | 5,413.574 ms | 8.438373 s |
| 다른 경로 500개 | 4,160.670 ms | 7.181690 s |

실제 self의 CPU/read/write/관측 간격이 유효했고 long_gap이 없었다. 실측 지연을 반영한 합성 follow-up도 완료창과 streak를 유지했다. query 28,000회, VM 158,872,000 steps, fullscan/sort 0을 확인했다. 이 측정은 합성 성숙 history에 대한 특정 환경의 결과이며 실제 장기 학습 정확도가 아니다. 3초 처리 예산은 달성하지 못했고 느린 PC·다른 부하에서의 15초 초과 가능성을 배제하지 않는다. 과거 성능 개선 실험과 이번 이동 후 측정의 실행 환경이 엄밀히 같다고 주장하지 않는다.

## 통합 실행과 저장

새 monitor_gui.exe의 headless 합성 모드 23회와 실제 모드 23회를 각각 별도 새 DB로 실행해 정상 종료했다. 실제 기록에서 317개 완료창/1,268 metric행, 창 길이 약 60.652~60.654초를 확인했다. DB integrity는 ok, foreign-key 오류는 0이었다. 새 DB의 준비 gate에 따라 learning으로 남았으며 여러 날 학습을 완료했다고 해석하지 않는다.

실제 관측에는 개인 프로세스명·경로가 있으므로 원본 DB·로그·검사 JSON은 공개에서 제외했다. 화면 상호작용, DPI·접근성·Pause/Resume·버튼·종료 시나리오는 이번에 수동 실행하지 않았다. headless 성공은 화면 검증을 대신하지 않는다.

## 공개 대상과 의존성

원본 연구·작업 로그와 측정 DB·WAL/SHM·화면 자료·이전 산출물은 로컬에 보존했다. 공개본은 소스·테스트·scripts·현재 계약·익명화 검증 요약과 SQLite의 빌드 필수 소스/헤더만 포함한다. 실제 커밋 대상의 파일 종류·개인 경로·일반 토큰 패턴, 문서 상대 링크, 소스 include, 크기를 점검한다. 패턴 검사는 모든 비밀정보의 부재를 수학적으로 증명하지 않는다.

SQLite 소스 SHA256과 로컬 원본 archive SHA3-256·파일 일치를 확인했으며 공식 다운로드 표시값과 대조했다. [PROVENANCE](../third_party/sqlite/PROVENANCE.md)에 출처·버전·public-domain 조건을 기록했다. 원본 ZIP과 미사용 sqlite3ext.h는 공개하지 않는다.

Git index의 공개 대상 54개 파일(약 10.6MB)만 새 폴더에 추출해 다시 빌드했다. 기존 산출물·개인 자료·vendor ZIP·미사용 헤더가 없는 상태에서 collector와 통합 GUI 빌드가 성공했고 core 65 / Windows 843 / monitor 9,160 checks 모두 통과했다. archive 없는 fresh 공개본에서 vendor 검증기도 소스 SHA256을 확인했다.

상대 문서 링크 52개, 로컬 include 30개, 이동 파일 14쌍의 바이트 일치, 실행 schema SQL 일치 및 공개 파일의 개인 경로·일반 토큰 패턴 검사를 확인했다. upstream SQLite의 원본 공백은 보존하고 프로젝트 파일은 diff whitespace 검사도 통과했다. 재현 명령은 [development](development.md), 경로 호환 영향은 [migration](migration.md)을 따른다.

## 남은 한계

실제 다일 학습·오탐/미탐, 장시간 성능·누수, 모든 부하에서의 지연 상한, 구형 Windows·배포 환경, GUI 수동 상호작용은 미검증이다. exact-path 식별은 파일 버전이나 앱 전체 역할을 보장하지 않는다. AI·외부 보안 프로그램 연동은 설계만 있고 실행하지 않았다. 프로젝트 자체 라이선스와 팀원 GitHub 계정·검토자 등록은 팀 결정이 필요하다.
