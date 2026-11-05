# 검증 결과와 남은 범위

이 문서는 **제출 준비에서 새로 실행한 확인**과 **2026-11-04~05 완료 작업의 기존 근거**를 구분한다. 과거 원시 DB·개인 경로·화면 캡처·대규모 보존본은 업로드하지 않는다. 합성 fixture와 독립 oracle은 함께 제출하여 현재 결과를 다시 검사할 수 있다. 검사 개수는 현실 시나리오 수나 탐지 정확도가 아니다.

## 이번 제출본 확인

2026-11-05 Windows x64/MSVC에서 Git index의 **214개 파일만** 새 디렉터리로 export했다. 기존 작업 폴더의 미추적 파일·설치 실행물·빌드 출력이 없는 상태에서 README의 기본 명령을 실행했다.

| 새로 실행한 확인 | 결과 |
|---|---|
| 수집기·monitor·prototype 기본 빌드 | 3개 모두 성공, C17 `/W4 /WX /O2 /MT /utf-8` |
| README 기본 시험 | 계산 65 / Windows 843 / 표시 85,398 / monitor 9,160 / grouping 1,152 / prototype 1,697, 실패 0 |
| 확장 `--profile build` | 22개 빌드 명령 모두 exit 0. 실패주입·독립 oracle·retention·source/history/window/ABI·benchmark의 동봉 입력과 링크 확인 |
| GUI 현재 명령·overlay fixture | 285 검사, 실패 0 |
| GUI 실제 worker+합성 SQLite fixture | 98 검사, 실패 0. 17 program과 18 저장·취소/프레임 경계를 확인 |
| SQLite offline 검증 | 동봉 C/H의 SHA-256 모두 일치. archive는 없이 소스만 검증 |
| 보존·인코딩 | 착수 제품 C/H·SQL·manifest 52개 raw SHA 변화 0. 공개 파일 UTF-8 확인, 빌드 중 입력 변화 0 |

기존 `.gitattributes`에 따라 Git blob은 C/H·문서 LF, checkout의 `.cmd`는 CRLF를 사용한다. 작업 트리의 raw SHA와 정규화된 blob SHA를 혼동하지 않고, 실제 export한 빌드 입력 해시를 최종 제출본에 연결했다. 결과를 적은 문서 수정 후에도 빌드 입력은 동일하다.

공개 대상에는 필수 fixture·literal ABI 기대값·실행 schema와 vendor를 포함했다. 비밀값 패턴·DB header·바이너리·개인 경로 및 문서 링크를 staged 내용으로 점검했다. 제품에 있던 EOF 빈 줄 3곳은 보존했으며 제출 준비에서 코드 정리를 섞지 않았다. 기존에 추적되던 과거 전용 mature probe 빌드/C와 SQL inspector는 현재 프로필이 사용하지 않아 Git에서만 제외하고 로컬에 남겼다.

이번에는 28 case 전체 실행이나 성능 측정을 반복하지 않았다. 확장 빌드 성공을 실패주입의 새 runtime 도달 증거나 GUI 전체 검증으로 보고하지 않는다. 관련 과거 도달·실패 근거는 아래에 따로 요약한다. 새 검증의 명령은 [README](../README.md), [개발 안내](development.md), [GUI fixture 안내](../tests/phase1-gui/README.md)에서 재현할 수 있다.

제출 준비의 제품 C/H·SQL은 착수 시 실제 미커밋·미추적 구현을 보존한 사본과 비교한다. 테스트 빌드의 유일한 동작 변경은 GUI 과거 기대 실패 실행물을 `historical-before` 선택 시에만 추가 빌드하도록 한 것이다. 현재 두 GUI 회귀와 oracle은 유지한다. 공개 문서·파일 선별·ignore 규칙을 정리했고 새로운 기능·정책·리팩터링은 추가하지 않았다.

## 기존 기능·오류 검증

2026-11-04 백엔드 2차 최종본은 제품·시험 22개 빌드와 28개 실행 case가 모두 exit 0이었다. 198개 입력·214개 산출 해시로 대상 버전을 연결했고 작성자와 분리한 reviewer가 구조·실패 경계와 최종 근거를 검토했다. 이번 제출 준비의 재실행 결과와 혼동하지 않는다.

| 범위 | 당시 확인한 내용 |
|---|---|
| 기본 | 계산 65, Windows 843, 표시 85,398, monitor 9,160, grouping 1,152, prototype 1,697 검사 통과 |
| 독립 Baseline | oracle 98,167 / OOM 98,168, 동일 digest `80c8301a884891dd`; 실제 cache OOM 16회, strict-past·정밀 start·날짜 대표·cutoff 보존 |
| 할당·실패 | 기존 calloc 3·malloc 12 실제 도달과 잔여 0; store open·source·history 부분 ROW/OOM과 prototype 모듈 실패 확인 |
| 새 경계 | source 614, history 506, 독립 window 158; Win64 공개 layout 동일·opaque 접근 경계 확인 |
| GUI | 숨김 컨트롤 1,426, 명령/overlay mock 285, 실제 worker+합성 SQLite 98. 이 결과는 전경 관리자 GUI 전체 시험이 아님 |
| 보관/replay | 통합 21,287, 독립 backlog 542, SQLite+retention만 링크한 138. 평가·종료 gap·요약만 남은 사건·반복 정리 확인 |

첫 단계에서 GUI 응답 누락과 소비 후 성공 표시 소실은 동일 mock oracle의 수정 전 기대 실패 2개에서 수정 후 통과로 바뀌었다. 실제 worker 시험에서는 17개 합성 program의 저장·용량 포화·취소와 frame watermark를 별도로 검사했다. mock 응답 수를 실제 DB 저장 수로 보고하지 않는다.

보관/replay와 episode-only 평가 고아는 동일 fixed 불변식 1,044개 중 수정 전 2개 실패, 수정 후 0개였다. 1,024개 recovered episode의 일부 요약만 남는 경우를 독립 재현해 retained replay의 종료 상태 복원을 보완했다. 보관 참조 보호 비용은 의도된 정합성 수정의 비용으로 분리한다.

## 기존 성능·메모리 비교와 불리한 결과

기준은 **1차 결함 수정 이후**이며 잘못된 참조를 삭제하던 과거 버전을 더 빠른 정상 기준으로 삼지 않았다. 같은 입력·빌드·권한·DB 조건에서 한 담당자가 직렬로 초기/성숙 500 인스턴스, same/mixed/distinct, core/prototype, 일반/완료 회차를 비교했다.

- 최종 12조건 snapshot/source/sidecar digest와 DB/WAL 크기가 일치했다. 최초 완료 회차 중앙값은 모두 **+0.60~5.18%**였고 선택한 3조건의 ABBA 후속은 **-2.32~+1.96%**였다. 통계적으로 무회귀라고 단정하지 않는다.
- 성숙 mixed prototype 완료 중앙값은 **753.6902 → 758.2131 ms**, distinct는 **3394.6461 → 3457.1140 ms**였다. 고유 경로가 많은 완료 회차의 3초 초과 비용이 남아 있다.
- core SQL 작업 수는 같았고 prototype 조회 순증가는 0이었다. source view는 완료창 500개에서 약 **20 KB**의 일시 배열을 추가하고 history 성공 반환은 최대 **512 B**를 복사한다. cache hit에는 SQL reader를 다시 호출하지 않는다.
- 132,000 decision·3,750 episode/평가의 보관 ABBA는 결과·삭제 ID·DB/WAL·SQL 작업이 같았다. 일반 trim은 **6.091~6.225 → 6.155~6.479 ms**, 만료 backlog는 **48.995~50.682 → 50.531~50.617 ms**였다.
- 관측한 private memory 전후 차이는 **-212,992~+401,408 B**, handles는 조건별 동일했다. 실제 다음 QPC 간격은 **3.101709~6.471457초**였다. 제한된 합성 실행이며 장기 누수 부재를 증명하지 않는다.

성능 도구는 함께 제출하지만 이번 업로드 준비에서는 장시간 성능이나 GUI 자동화를 반복하지 않는다. 과거 GUI 계측 수치를 최종 제품의 quiet GUI 합격 근거로 재사용하지 않는다.

## 한국어 설명 주석

2026-11-05 제품 49개 C/H와 관련 시험 12개 C/H의 설명을 정리했다. 역할·입력/출력·유효 조건·QPC/UTC·소유권·실패·commit·worker/UI 전달의 전제를 중심으로 설명했다. 주석 전후 비주석 토큰 101,328개와 문자열/문자 리터럴이 같았고 UTF-8·기존 BOM/개행을 확인했다. 정상 3개 빌드는 성공했고 이 주석 단계에서 runtime·성능 전체 회귀는 반복하지 않았다.

## 남은 한계

- 실제 전경 관리자 GUI의 전체 상호작용, 외부 UI 조회가 제거된 최종 GUI 성능, DPI/접근성 전체 조합은 별도 검증 대상이다.
- 실제 여러 날 학습의 오탐·미탐·사용자 효용, 장기 누수, 여러 writer와 모든 SQLite 내부 실패를 포괄 검증하지 않았다.
- prototype의 제한된 private context, GUI 전역 상태·worker/화면 결합, core와 sidecar의 별도 commit은 남는다. core commit 후 sidecar 실패는 부분 성공이며 core를 다시 ingest하지 않는다.
- 같은 회차에 중복 신원 tuple을 주는 입력의 기존 first-row 연결 한계는 해결하지 않았다. 현재 source 계약은 이 한계를 숨기지 않으며 별도 회귀가 기존 동작을 관측한다.
- 합성 관리자/숨김 GUI fixture, 기본 시험, 실제 DB 저장, 실제 화면 전체 검증은 서로 다른 범위다. 미실행이나 실패를 정상으로 표기하지 않는다.

재현 가능한 명령은 [개발 안내](development.md), 시험 구성은 [tests 안내](../tests/README.md), 경계·소유권은 [아키텍처](architecture.md)와 [source 계약](contracts/source.md)을 따른다.
