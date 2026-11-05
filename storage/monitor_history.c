/* Baseline의 날짜별 과거 선택 SQL. 학습 가능한 과거 창에서 종료 분마다 가장 늦은 창을
 * end_utc, id 순으로 날짜별 최대 64개 고르며 정밀 시작시각과 회차 cutoff를 함께 적용한다. */
#include <math.h>
#include <string.h>
#include "monitor_store_private.h"
#include "monitor_history.h"
static const char history_sql[] =
"WITH RECURSIVE picks(id,end_utc,value,n) AS ("
            "SELECT NULL,?4,NULL,0 UNION ALL "
            "SELECT w.id,w.end_utc,(SELECT value FROM aggregate_metric WHERE window_id=w.id AND metric=?1),p.n+1 "
            "FROM picks p JOIN aggregate_window w ON w.id=("
            "SELECT x.id FROM aggregate_window x JOIN aggregate_metric m ON m.window_id=x.id AND m.metric=?1 "
            "AND m.eligible=1 AND m.value IS NOT NULL AND m.value>=0 AND m.value<=1.7976931348623157e308 "
            "WHERE x.program_id=?2 AND x.version=1 AND x.end_utc>=?3 "
            "AND x.end_utc<min((p.end_utc/600000000)*600000000,?5) AND x.id<=?6 "
            "ORDER BY x.end_utc DESC,x.id DESC LIMIT 1) WHERE p.n<64) "
            "SELECT value,end_utc FROM picks WHERE n>0";
int monitor_history_prepare(MonitorStore *store)
{ return monitor_store_prepare(store, history_sql, &store->baseline_query); }
int monitor_history_select_day(MonitorStore *store, const MonitorHistoryDayRequest *request,
    double values[BASELINE_SAMPLES_PER_DAY], unsigned *count)
{
    double selected[BASELINE_SAMPLES_PER_DAY];
    unsigned n = 0;
    sqlite3_stmt *s;
    int rc;
    if (count) *count = 0;
    if (!store || !request || !values || !count) return 0;
    s = store->baseline_query;
    sqlite3_bind_int(s, 1, (int)request->metric);
    sqlite3_bind_int64(s, 2, request->program_id);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)request->lower);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)request->upper);
    sqlite3_bind_int64(s, 5, (sqlite3_int64)request->exact_start);
    sqlite3_bind_int64(s, 6, request->cutoff);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        double value = sqlite3_column_double(s, 0);
        if (n < BASELINE_SAMPLES_PER_DAY && isfinite(value) && value >= 0) selected[n++] = value;
    }
    if (rc != SQLITE_DONE) monitor_store_set_error(store, "baseline query");
    sqlite3_reset(s);
    sqlite3_clear_bindings(s);
    if (rc != SQLITE_DONE) return 0;
    memcpy(values, selected, n * sizeof(*values));
    *count = n;
    return 1;
}
