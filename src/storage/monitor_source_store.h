#ifndef MONITOR_SOURCE_STORE_H
#define MONITOR_SOURCE_STORE_H
#include "monitor_store.h"
#include "../engine/monitor_source.h"
/* engine이 cookie·epoch·출력 동일성을 검증한 뒤 호출하는 저장소 조회 경계.
 * statement는 store가 관리하며 완전히 조회된 view 메타데이터만 호출자에게 넘긴다. */
int monitor_store_source_read(MonitorStore *store, const MonitorCommitRef *validated_ref,
    const MonitorSnapshot *snapshot, MonitorCommittedWindows *out);
int monitor_store_source_create_pair(MonitorStore *store, const char *pair);
#endif
