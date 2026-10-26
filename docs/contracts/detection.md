# 개인화 Baseline과 이상 후보 계약

실제 통계·판정은 `detection/baseline.c`, 내부 함수 계약은 `detection/baseline.h`다. 관측창은 `engine/monitor.c`, prepared SQL과 인덱스는 storage가 관리한다. 공개 결과 자료형은 [monitor.h](../../include/monitor.h)다.

## 비교 대상과 준비 조건

같은 정확한 실행 경로의 개별 프로세스 관측창 분포를 비교한다. 앱 전체/모든 자식 프로세스 합계가 아니다. 연속한 실제 관측 간격을 합쳐 60~75초 창을 만든다. 유효한 행 간격은 0초 초과 15초 이하다. UTC 역행, 긴 공백, 신원 변화 등은 미완료 창과 지속성을 초기화한다.

CPU/I/O는 각 실제 구간으로 가중한 평균이며 창의 75~125% coverage를 요구한다. RAM은 유효 표본의 산술평균이며 최소 15표본이다. 필드별 품질과 결측을 독립적으로 판단한다.

현재 창 시작 날짜 포함 최근 14 UTC 날짜에서 metric별 eligible 과거만 선택한다. 매 날짜 **서로 다른 종료 분마다 가장 늦은 end_utc, 동률이면 가장 큰 ID의 창** 한 개를 선택하고 최대 64분을 채운다. NULL·비유한값·음수는 선택 전 제외하고 유효한 0은 유지한다. 현재 창 시작은 분으로 반올림하지 않는다.

준비 gate는 **60개 창 + 서로 다른 3 UTC 날짜 + 60개 종료 분**을 모두 충족하는 것이다. 다중 인스턴스가 같은 분의 행을 늘려 날짜 cap을 소진하지 않는다. 선택값은 해당 대표 창의 값이며 분 평균이나 앱 합계로 바꾸지 않는다. 날짜별 최대 64회 선택은 bounded recursive SQL로 수행하고 C에서는 날짜별 한 번 수신한다.

## 통계와 상태

```text
median = median(과거 값)
MAD = median(abs(과거 값 - median))
upper = median + max(3 * 1.4826 * MAD, 0.5 * median, floor)
floor = CPU 5 percentage points / RAM 128 MiB / read·write 각각 1 MiB/s
```

| 상태 | 의미 |
|---|---|
| unavailable | 신원/필드/관측창 품질 등 비교 근거가 없음 |
| learning | 유효한 과거가 준비 gate에 미달 |
| normal | 준비된 해당 metric이 상한 이하 |
| attention | 같은 metric이 상한을 1~2개 창 연속 초과 |
| candidate | 같은 metric이 3개 창 연속 초과한 자원 이상 후보 |

준비 후 초과 metric은 이후 Baseline 학습에서 제외한다. 정상·결측·공백·신규 인스턴스는 지속성을 끊는다. 학습 중 품질 충족 표본은 학습에 들어간다. 현재 표본은 비교를 마친 후 기록하며, 회차 고정 cutoff와 strict-past 조건을 모두 유지한다. 피드백은 학습값·상한·후보 사실을 바꾸지 않는다.

이 상수와 gate는 프로젝트의 초기 정책이다. 합성 회귀는 계약 검증이며 실제 여러 날 학습, 오탐·미탐 정확도, 모든 부하에서의 지연 상한을 입증하지 않는다. 정상적인 패턴 변화가 계속 제외되면 과거 만료 후 learning으로 돌아갈 수 있다. 신규·결측·고사용·비전경은 악성 또는 불필요의 증거가 아니다.

필수 회귀: `tests/integration/test_monitor.c`, 기존 별도 검토 테스트 `--minute-shadow --expect-ready`, `--minute-selection`. [개발 명령](../development.md)을 따른다. 통계 배경 자료인 [NIST Measures of Scale](https://www.itl.nist.gov/div898/handbook/eda/section3/eda356.htm)은 MAD의 정의를 설명하며 이 프로젝트의 임계 상수·정확도를 인증하는 근거는 아니다.
