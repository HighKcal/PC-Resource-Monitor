/* Each probe is compiled separately. The no-PROBE baseline must compile. A
 * rejection is meaningful only if it reports incomplete/undeclared state, not
 * a missing header or unrelated syntax problem. No product implementation is
 * textually included. */
#if defined(REVIEW_HEADER_STORE)
#include "monitor_store.h"
#elif defined(REVIEW_HEADER_BASELINE)
#include "baseline.h"
#elif defined(REVIEW_HEADER_SOURCE)
#include "monitor_source.h"
#else
#include "monitor.h"
#endif

int main(void)
{
#if defined(REVIEW_PROBE_ENGINE_LAYOUT)
    return (int)sizeof(struct MonitorEngine);
#elif defined(REVIEW_PROBE_STORE_LAYOUT)
    return (int)sizeof(struct MonitorStore);
#elif defined(REVIEW_PROBE_INSTANCE_LAYOUT)
    return (int)sizeof(MonitorInstance);
#elif defined(REVIEW_PROBE_SQLITE_LAYOUT)
    sqlite3 *connection = 0;
    return connection != 0;
#else
    return 0;
#endif
}
