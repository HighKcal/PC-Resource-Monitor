#ifndef EXPERIMENT_RETENTION_INTERNAL_H
#define EXPERIMENT_RETENTION_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include "sqlite3.h"

/* 쓰기 가능한 실험 DB connection을 빌려 동기적으로 보관 정리만 수행한다.
 * 호출자는 열린 transaction을 소유하며 검증된 UTC FILETIME tick(0..INT64_MAX)을 넘긴다.
 * 이 함수는 transaction을 시작·commit·rollback하거나 connection을 닫지 않고
 * 호출자 상태도 소유하지 않는다. 실패 시 부분 변경의 rollback은 호출자 책임이다.
 * 여기서 생성한 statement는 반환 전에 모두 finalize하며 기존 호출자 statement는 건드리지 않는다.
 * 성공은 1, SQLite 실패는 0을 반환한다. 선택적 오류 버퍼는 호출 중에만 빌리고 성공 시 유지한다.
 * 공유 private context 및 Experiment/core/Track에 의존하지 않는다. */
int experiment_retention_trim(sqlite3 *db, uint64_t utc, char *error, size_t error_size);

#endif
