#include "resource_core.h"

#include <math.h>
/* OS 호출 없이 원시 카운터·시각을 합성해 rc_core_update의 차분 계약을 검사한다.
 * CPU·I/O·RAM의 유효성을 따로 확인하며, 기대값은 입력 수치에서 직접 정한다. */
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 0.00000001)

static RcRawSample sample(void)
{
    RcRawSample raw;
    memset(&raw, 0, sizeof(raw));
    raw.pid = 42;
    raw.utc_100ns = 100000000;
    raw.creation_100ns = 10000000;
    raw.identity_valid = raw.cpu_valid = raw.io_valid = raw.memory_valid = 1;
    raw.cpu_100ns = 100000000;
    raw.read_bytes = 10000;
    raw.write_bytes = 20000;
    raw.working_set_bytes = 123456;
    raw.observation_qpc = raw.cpu_qpc = raw.io_qpc = 1000;
    return raw;
}

static void advance(RcRawSample *raw, int64_t ticks)
{
    raw->observation_qpc += ticks;
    raw->cpu_qpc += ticks;
    raw->io_qpc += ticks;
    raw->utc_100ns += (uint64_t)ticks * 10000;
}

static void update(RcRawSample *raw, RcState *state, ResourceData *out)
{
    RcState next;
    rc_core_update(raw, state, 1000, 8, 15.0, &next, out);
    *state = next;
}

int main(void)
{
    RcRawSample raw = sample();
    RcState state;
    ResourceData out;
    rc_core_update(&raw, NULL, 1000, 8, 15.0, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_FIRST_SAMPLE);
    CHECK(isnan(out.cpu_percent) && isnan(out.io_read_bytes_per_second));
    CHECK((out.valid_fields & RESOURCE_VALID_CPU) == 0);
    CHECK((out.valid_fields & RESOURCE_VALID_MEMORY) != 0);
    CHECK(out.working_set_bytes == 123456);
    CHECK(NEAR(out.elapsed_seconds, 9.0));
    advance(&raw, 4000);
    raw.cpu_100ns += 40000000;
    raw.read_bytes += 800;
    raw.write_bytes += 1200;
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);
    CHECK(NEAR(out.cpu_percent, 12.5)); /* CPU 누적 4초 / 관측 4초 / 논리 CPU 8개 = 전체 용량의 12.5%. */
    CHECK(NEAR(out.io_read_bytes_per_second, 200.0));
    CHECK(NEAR(out.io_write_bytes_per_second, 300.0));
    CHECK(NEAR(out.observation_interval_seconds, 4.0));
    CHECK(NEAR(out.cpu_interval_seconds, 4.0));
    CHECK(NEAR(out.io_interval_seconds, 4.0));

    /* 같은 누적 카운터는 유효한 측정값 0이며 결측과 다르다. */
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID && out.cpu_percent == 0.0);
    CHECK(out.io_read_status == RESOURCE_DELTA_VALID &&
        out.io_read_bytes_per_second == 0.0);
    CHECK(out.io_write_status == RESOURCE_DELTA_VALID &&
        out.io_write_bytes_per_second == 0.0);

    /* 필드별 실패 → 복구 첫 표본의 기준 재설정 → 다음 구간의 유효 차분을 확인한다. */
    advance(&raw, 4000);
    raw.cpu_valid = 0;
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_QUERY_FAILED && isnan(out.cpu_percent));
    CHECK(out.io_read_status == RESOURCE_DELTA_VALID);
    CHECK((out.valid_fields & RESOURCE_VALID_MEMORY) != 0);
    raw.cpu_valid = 1;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_QUERY_FAILED);
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);
    raw.io_valid = 0;
    raw.memory_valid = 0;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.io_read_status == RESOURCE_DELTA_QUERY_FAILED);
    CHECK(out.io_write_status == RESOURCE_DELTA_QUERY_FAILED);
    CHECK((out.valid_fields & RESOURCE_VALID_MEMORY) == 0);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);
    raw.io_valid = raw.memory_valid = 1;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.io_read_status == RESOURCE_DELTA_QUERY_FAILED);
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.io_read_status == RESOURCE_DELTA_VALID);

    /* PID 재사용: PID는 같아도 생성시각이 달라지면 다른 인스턴스다. */
    ++raw.creation_100ns;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_IDENTITY_CHANGED);
    CHECK(out.io_read_status == RESOURCE_DELTA_IDENTITY_CHANGED);
    CHECK(isnan(out.cpu_percent));
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);
    /* 새 PID도 다른 인스턴스의 이전 카운터를 빌려서는 안 된다. */
    ++raw.pid;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_FIRST_SAMPLE);

    raw.identity_valid = 0;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    CHECK(out.io_read_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    CHECK((out.valid_fields & RESOURCE_VALID_MEMORY) != 0);
    CHECK((out.valid_fields & RESOURCE_VALID_ELAPSED) == 0);
    raw.identity_valid = 1;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_IDENTITY_UNAVAILABLE);
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);

    /* 15초는 허용하며, 15초를 초과한 공백에서만 차분이 무효다. */
    advance(&raw, 15000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);
    CHECK(NEAR(out.cpu_interval_seconds, 15.0));
    advance(&raw, 15001);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_LONG_GAP);
    CHECK(out.io_read_status == RESOURCE_DELTA_LONG_GAP);
    CHECK(isnan(out.cpu_percent) && isnan(out.io_interval_seconds));
    CHECK((out.valid_fields & RESOURCE_VALID_OBSERVATION_INTERVAL) == 0);
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID);

    raw.cpu_100ns = 1;
    raw.read_bytes = 1;
    ++raw.write_bytes;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_COUNTER_ROLLBACK);
    CHECK(out.io_read_status == RESOURCE_DELTA_COUNTER_ROLLBACK);
    CHECK(out.io_write_status == RESOURCE_DELTA_VALID);
    CHECK(isnan(out.cpu_percent) && isnan(out.io_read_bytes_per_second));
    raw.cpu_100ns += 40000000;
    raw.read_bytes += 800;
    raw.write_bytes = 1;
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID && NEAR(out.cpu_percent, 12.5));
    CHECK(out.io_read_status == RESOURCE_DELTA_VALID);
    CHECK(out.io_write_status == RESOURCE_DELTA_COUNTER_ROLLBACK);
    advance(&raw, 4000);
    update(&raw, &state, &out);
    CHECK(out.io_write_status == RESOURCE_DELTA_VALID);

    /* CPU와 I/O 조회 시점이 다르면 요청 주기가 아닌 각 실제 간격으로 나눈다. */
    advance(&raw, 4000);
    raw.io_qpc -= 1000;
    raw.cpu_100ns += 40000000;
    raw.read_bytes += 600;
    update(&raw, &state, &out);
    CHECK(NEAR(out.cpu_percent, 12.5));
    CHECK(NEAR(out.io_read_bytes_per_second, 200.0));
    CHECK(NEAR(out.cpu_interval_seconds, 4.0) && NEAR(out.io_interval_seconds, 3.0));
    /* QPC가 멈추거나 역행하면 0 나눗셈이나 unsigned 뺄셈의 역전 없이 무효화한다. */
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_CLOCK_INVALID);
    CHECK(out.io_read_status == RESOURCE_DELTA_CLOCK_INVALID);
    raw.cpu_qpc -= 1;
    raw.io_qpc -= 1;
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_CLOCK_INVALID);
    advance(&raw, 4000);
    raw.utc_100ns = raw.creation_100ns - 1;
    update(&raw, &state, &out);
    CHECK(out.cpu_status == RESOURCE_DELTA_VALID); /* 기록용 UTC의 점프는 차분 계산용 QPC에 영향을 주지 않는다. */
    CHECK((out.valid_fields & RESOURCE_VALID_ELAPSED) == 0 && isnan(out.elapsed_seconds));

    CHECK(rc_foreground_for_pid(42, 0) == RESOURCE_FOREGROUND_UNKNOWN);
    CHECK(rc_foreground_for_pid(42, 42) == RESOURCE_FOREGROUND);
    CHECK(rc_foreground_for_pid(42, 99) == RESOURCE_BACKGROUND);
    CHECK(strcmp(resource_delta_status_string(RESOURCE_DELTA_LONG_GAP), "long_gap") == 0);
    printf("PASS core: %u checks (shared production calculation/state transition)\n", checks);
    return 0;
}
