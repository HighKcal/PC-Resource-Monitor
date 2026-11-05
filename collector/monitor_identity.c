/* 원시 수집과 별도로 경로를 조회하되 같은 핸들의 생성시각으로 PID 재사용을 확인한다.
 * 성공한 경로 사본은 호출자가 소유하며 파일 버전의 동일성까지 확인하지는 않는다. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "monitor.h"

static uint64_t filetime_value(FILETIME t)
{
    return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
}
uint64_t monitor_utc_now(void)
{
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return filetime_value(t);
}
char *monitor_query_path(uint32_t pid, uint64_t expected_creation, uint32_t *error)
{
    HANDLE h;
    FILETIME creation, exit_time, kernel, user;
    wchar_t *wide;
    char *path = NULL;
    DWORD length = 32768;
    int bytes;
    if (error) *error = ERROR_INVALID_PARAMETER;
    if (!expected_creation) return NULL;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { if (error) *error = GetLastError(); return NULL; }
    if (!GetProcessTimes(h, &creation, &exit_time, &kernel, &user)) {
        if (error) *error = GetLastError();
        CloseHandle(h); return NULL;
    }
    if (filetime_value(creation) != expected_creation) {
        if (error) *error = ERROR_INVALID_DATA;
        CloseHandle(h); return NULL;
    }
    wide = malloc((size_t)length * sizeof(*wide));
    if (!wide) { if (error) *error = ERROR_NOT_ENOUGH_MEMORY; CloseHandle(h); return NULL; }
    if (!QueryFullProcessImageNameW(h, 0, wide, &length)) {
        if (error) *error = GetLastError();
    } else {
        wide[length] = L'\0';
        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
        if (bytes > 0) {
            path = malloc((size_t)bytes);
            if (path && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                wide, -1, path, bytes, NULL, NULL)) {
                free(path); path = NULL;
            }
        }
        if (error) *error = path ? 0 : (bytes > 0 ? ERROR_NOT_ENOUGH_MEMORY : GetLastError());
    }
    free(wide); CloseHandle(h); return path;
}
