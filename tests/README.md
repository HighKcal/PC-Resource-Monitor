# 제출본 빌드와 회귀 시험

저장소 루트의 PowerShell에서 실행한다. Windows x64, Visual Studio의 C++ 데스크톱 도구와 Windows SDK가 필요하다. 아래 프로필 실행기는 Python 3 표준 라이브러리만 사용한다. 제품의 기본 `.cmd` 빌드에는 Python이 필요하지 않다.

```powershell
python scripts/test-profiles.py --profile build --output build-submission-check
python scripts/test-profiles.py --profile all --built-from build-submission-check --output build-submission-regression
```

출력 이름은 존재하지 않는 경로로 지정한다. 실행기는 기존 결과를 덮지 않으며 입력 SHA-256, 명령, 로그, 산출물 해시와 `result.json`을 기록한다. `--built-from`은 성공한 build 프로필과 동일한 입력·산출물 해시를 요구한다. 첫 명령 이후 소스나 시험 스크립트를 바꾸면 새 build 출력으로 다시 시작한다.

| 프로필 | 확인 범위 |
| --- | --- |
| `build` | 제품·기본 시험·추가 회귀·실패주입·벤치마크 22개 빌드 명령. 실행 파일은 실행하지 않음 |
| `basic` | 수집 계산·Windows 수집·표시 변환·통합 monitor·grouping·prototype·독립 window 모듈 |
| `policy` | 고정 Win64 공개 레이아웃·history 범위/부분 조회·minute 선택·독립 Baseline oracle |
| `failure` | store/source/epoch·calloc/malloc·cache OOM·prototype 모듈·보관 rollback/소유권 |
| `prototype` | 기본/독립 prototype·기존 중복 신원 관측·모듈 실패·보관/replay 회귀 |
| `gui` | 숨김 Win32 컨트롤·mock feedback·실제 worker와 합성 SQLite feedback |
| `all` | 위 실행 프로필 전체. `--built-from`이 없으면 먼저 전체 빌드 |

회귀마다 별도 `cases/` 작업 경로와 TEMP/TMP를 사용한다. 현재 프로필은 과거 `build-*` 보존본이나 실사용 DB를 입력으로 요구하지 않는다. `tests/support/`의 fixture C/H, `phase2-core/public-layout-win64.txt`, `review-native-ui/test_native_ui_review.c`는 다른 시험에서 직접 소비하므로 함께 보존한다. 시험 수나 합성 입력 통과는 실제 장기 학습의 정확도·사용자 효용을 뜻하지 않는다.

제품 GUI 실행, UAC 조작, 전경 GUI 상호작용, quiet GUI 성능과 장기 누수는 이 프로필의 검증 범위 밖이다. 벤치마크 링크 성공은 성능 측정 결과가 아니다. `test_windows`는 실제 Windows API와 자신이 만든 자식 프로세스를 사용한다. GUI 시험은 제품 진입점을 실행하지 않으며 테스트용 창·worker·합성 DB의 범위는 각 fixture에 명시되어 있다.

## 과거 비교와 선택 진단

제출본의 필수 실행 경로는 `scripts/test-profiles.py`다. 과거 단계의 전후 비교·개인 환경용 launcher·해시 감사·계측 준비 도구 일부는 별도 `build-*` 보존본을 요구하며 공개본에서 제외할 수 있다. 문서의 과거 경로와 해시는 당시 결과를 식별하며 새 checkout의 재현 성공을 뜻하지 않는다.

`phase1-gui/build.cmd`의 네 번째 인수 `historical-before`는 과거 GUI 보존본을 이용한 기대 실패 재현을 명시적으로 추가한다. 현재 두 회귀의 빌드는 이 인수를 사용하지 않는다. 자세한 명령과 기대 실패는 [GUI feedback 회귀 설명](phase1-gui/README.md)에 있다. 역사 입력의 해시·oracle 기대값을 현재 코드에 맞춰 바꾸어 과거 재현으로 보고하지 않는다.

성능 도구와 DB 진단은 명시한 새 합성 출력에서 한 담당자가 직렬 실행한다. 기존 실사용 DB와 개인 측정 로그는 제출하거나 시험 입력으로 사용하지 않는다. 최신 구조·검증 범위와 남은 한계는 [개발 명령](../docs/development.md), [검증 문서](../docs/validation.md)를 참고한다.
