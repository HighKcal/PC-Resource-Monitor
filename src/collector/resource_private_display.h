#ifndef RESOURCE_PRIVATE_DISPLAY_H
#define RESOURCE_PRIVATE_DISPLAY_H
#include "resource_display.h"

/* 한 회차의 화면 귀속용 전용 Working Set(bytes)을 선택적으로 함께 수집한다.
 * private_ws가 NULL이면 이 배열을 요청하지 않는다. 요청할 때는 호출 전 *private_ws=NULL이어야
 * 하며, out과 선택적 display도 비어 있어야 한다. 한 회차만 수집하고 대기하지 않는다.
 *
 * 성공 시 (*private_ws)[i]는 out->items[i]와 같은 행의 값이다. UINT64_MAX는 측정 불가이며
 * 유효한 0과 다르다. out->count개 값을 호출자가 소유하고 free(*private_ws)로 따로 해제한다.
 * resource_batch_free/resource_display_batch_free는 이 배열을 해제하지 않는다. 다음 요청 전에는
 * 포인터를 다시 NULL로 둔다. 수집 행이 0개면 성공해도 배열은 NULL일 수 있다.
 *
 * 플랫폼·할당 실패 시 부분 배열을 내부에서 해제해 요청한 *private_ws는 NULL, out/display는 빈 배치로
 * 만들고 이전 카운터 상태도 비운다. RESOURCE_INVALID_ARGUMENT로 거부할 때는 입력 포인터·배치와
 * 기존 수집 상태를 해제하거나 바꾸지 않는다. win32_error를 받았으면 이때도 0으로 초기화한다.
 * 이 별도 배열은 표시용이다. ResourceData의 RAM과 저장 DB는 기존 Working Set 의미를 유지하며
 * 전용 Working Set 값을 ResourceData나 DB에 추가하지 않는다. */
ResourceStatus resource_collect_with_private_ws(ResourceCollector *collector,
    ResourceBatch *out, ResourceDisplayBatch *display, uint64_t **private_ws,
    uint32_t *win32_error);
#endif
