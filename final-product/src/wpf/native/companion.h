#ifndef PC_MONITOR_WPF_COMPANION_H
#define PC_MONITOR_WPF_COMPANION_H
#include <windows.h>
#include <stdint.h>
#include "notification_policy.h"
#define PC_COMPANION_API __declspec(dllexport)

/* 프로그램 정보 API는 동기 호출이며 같은 handle의 호출을 겹치지 않게 한다.
 * ProgramInfoWindow는 collect/execute를 Task.Run에서, 짧은 상태·표시 조회를 작업 중이
 * 아닐 때 UI에서 호출한다. 반환 UTF-8 문자열은 다음 작업 전까지 빌리므로 즉시 복사한다.
 * 호출자는 작업 완료까지 context와 취소 이벤트를 유지한다. 수집 snapshot은 보관하지 않는다. */
PC_COMPANION_API void *pc_program_collect(uint32_t pid,uint64_t creation,unsigned session,
    int synthetic,const char *path,HANDLE cancel);
PC_COMPANION_API void pc_program_destroy(void *context);
PC_COMPANION_API int pc_program_current(void *context);
PC_COMPANION_API const char *pc_program_text(void *context,unsigned intent,int details);
/* preview는 전송하지 않는다. op=0은 공개 자료 조회, op=2는 원문 한 문장 번역이다.
 * execute는 같은 현재 미리보기를 요구한다. op=1의 명시적 URL 열기는 별도 경로다. */
PC_COMPANION_API const char *pc_program_preview(void *context,unsigned intent,int op);
PC_COMPANION_API int pc_program_preview_ready(void *context);
PC_COMPANION_API int pc_program_execute(void *context,unsigned intent,int op,unsigned source,HANDLE cancel);
PC_COMPANION_API void pc_program_clear_preview(void *context);
PC_COMPANION_API unsigned pc_program_source_count(void *context);
PC_COMPANION_API const char *pc_program_source(void *context,unsigned index,int url);

/* 모든 Shell 호출과 HWND 콜백은 WPF Dispatcher가 맡고 정책은 bridge worker에 남긴다.
 * event는 관리 경계에서 독립 값으로 복사하며 이 동기 요청 동안만 포인터로 빌린다. */
PC_COMPANION_API void *pc_tray_create(HWND owner,UINT callback_message);
PC_COMPANION_API int pc_tray_ready(void *context);
PC_COMPANION_API int pc_tray_request(void *context,const NotificationEvent *event,uint64_t now_ms);
PC_COMPANION_API int pc_tray_message(void *context,UINT message,WPARAM wp,LPARAM lp,
    int enabled,int *action,uint64_t *id);
PC_COMPANION_API uint64_t pc_tray_tick(void *context,uint64_t now_ms);
PC_COMPANION_API void pc_tray_cancel(void *context,uint64_t id);
PC_COMPANION_API void pc_tray_destroy(void *context);
#endif
