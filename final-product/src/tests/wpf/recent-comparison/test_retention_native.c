/* WPF 피드백 동선의 보관 정리만 28일 뒤로 진행한다. 실제 시간·실사용 DB·제품 ABI를
 * 바꾸지 않으며 나머지 bridge/engine/storage는 같은 제품 구현과 오브젝트를 사용한다. */
#include <stdint.h>
static uint64_t fixture_clock = UINT64_C(134390000000000000);
uint64_t recent_fixture_utc_now(void) { return fixture_clock; }
#define monitor_utc_now recent_fixture_utc_now
#include "../../../wpf/native/bridge.c"
#undef monitor_utc_now
#include "../../support/monitor_fixture.h"
static unsigned fixture_recent_queries;
static int fixture_trace(unsigned event, void *context, void *statement, void *sql)
{
    (void)event; (void)context; (void)statement;
    if (strstr((const char *)sql, "FROM aggregate_window w JOIN aggregate_metric m") ||
        strstr((const char *)sql, "FROM aggregate_window x INDEXED BY aggregate_program_time"))
        ++fixture_recent_queries;
    return 0;
}
BRIDGE_API int bridge_recent_fixture_clock(Bridge *b, uint64_t utc)
{
    if (!worker(b) || !b->engine || !utc) return 0;
    fixture_clock = utc;
    fixture_recent_queries = 0;
    return sqlite3_trace_v2(monitor_fixture_db(b->engine), SQLITE_TRACE_STMT, fixture_trace, NULL) == SQLITE_OK;
}
BRIDGE_API unsigned bridge_recent_fixture_queries(Bridge *b)
{
    return worker(b) ? fixture_recent_queries : UINT32_MAX;
}
