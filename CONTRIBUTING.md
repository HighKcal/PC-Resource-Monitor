# Fork 기반 협업 안내

원본은 [HighKcal/PC-Resource-Monitor](https://github.com/HighKcal/PC-Resource-Monitor), 통합 대상은 `main`이다. 먼저 [README](README.md), [팀원별 읽기 순서](docs/team-guide.md), [아키텍처](docs/architecture.md)를 읽는다. 담당 지정은 앞으로의 관리 책임이며 기존 코드의 작성자·기여자를 뜻하지 않는다.

## 1. Fork와 Clone

GitHub 원본 저장소에서 **Fork**를 눌러 자신의 계정에 만든 후 PowerShell에서 실행한다. `<your-github-name>`은 자신의 실제 GitHub 사용자명으로 바꾼다.

```powershell
git clone https://github.com/<your-github-name>/PC-Resource-Monitor.git
cd PC-Resource-Monitor
git remote add upstream https://github.com/HighKcal/PC-Resource-Monitor.git
git remote -v
git switch -c collector/example-change
```

`origin`은 개인 Fork, `upstream`은 원본이다. 브랜치 접두어는 `collector/`, `storage/`, `detection/`, `gui/`, 공통 수정은 `integration/` 또는 `docs/`를 사용한다. 예시 브랜치 이름은 실제 변경 목적에 맞춘다.

## 2. 개발과 검증

담당 폴더와 연결 테스트를 수정한다. [개발 명령](docs/development.md)을 따라 깨끗한 별도 출력 폴더에서 빌드하고 회귀 테스트를 실행한다. 공통 헤더, 내부 연결 헤더, DB 스키마, 관측창, Baseline 정책, worker 흐름을 바꾸기 전에는 영향받는 담당자와 PR 또는 Issue에서 계약을 협의한다.

특히 `include/`는 공개 API, `engine/monitor_internal.h`는 내부 연결 규격이다. `storage/monitor_schema.h`와 `storage/schema.sql`은 일치해야 한다. 알고리즘이 SQLite 조회 조건을 바꾸면 storage와 detection 담당이 함께 검토한다. 현재 회차 누출, 결측을 0으로 처리, PID만으로 연결하는 변경을 금한다.

개인 PC의 DB, 프로세스 목록, 경로, CSV, 화면 캡처, 키·토큰, 실행 파일을 커밋하지 않는다. 합성 테스트 데이터와 실제 장기 학습을 구분한다. Windows 실행 정책이 차단하면 정책을 변경하거나 우회하지 말고 차단된 검증을 기록한다.

## 3. Commit, Push, Pull Request

```powershell
git status --short
git diff
git add collector tests/collector
git diff --cached --stat
git diff --cached
git commit -m "Describe the concrete collector change"
git push -u origin collector/example-change
```

`git add`의 폴더와 브랜치 이름은 자신의 변경에 맞춘다. GitHub에서 **base repository = HighKcal/PC-Resource-Monitor, base = main**, **head repository = 개인 Fork, compare = 작업 브랜치**로 PR을 만든다.

PR에는 변경 목적과 전후 동작, 수정 영역, 공통 API·DB·단위·소유권·학습 정책 영향, 실제 실행 명령과 결과, 실행하지 못한 항목을 적는다. [PR 양식](.github/pull_request_template.md)을 사용한다. 담당자 검토와 회귀 확인 후 원본에 병합한다. 팀원의 GitHub ID가 확인되기 전에는 임의의 CODEOWNERS나 reviewer 계정을 등록하지 않는다.

## 4. 원본 변경 동기화

작업 내용은 먼저 커밋하거나 보존한다. 아래는 새 기능 작업 전 `main` 동기화 절차다.

```powershell
git fetch upstream
git switch main
git merge --ff-only upstream/main
git push origin main
git switch -c gui/next-change
```

진행 중인 기능 브랜치에는 `git merge upstream/main`으로 원본 변경을 반영하고 충돌을 검토한 뒤 다시 테스트한다. `--ff-only`가 거부되면 Fork의 추가 커밋을 조사한다. 이력 삭제, force push, 무분별한 clean으로 해결하지 않는다.
