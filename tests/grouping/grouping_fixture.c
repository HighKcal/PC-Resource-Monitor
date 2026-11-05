#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdint.h>

/* Dedicated test processes only. Marker files request NORMAL exits. No process
 * is terminated by this program, and all work stays in the given directory. */
int wmain(int argc,wchar_t **argv)
{
    wchar_t directory[2048]=L"",exe[32768],command[32768],file[4096],marker[4096];
    unsigned role=0,lifetime=300000; int i; uint64_t start; FILETIME created,exit,kernel,user;
    void *memory; HANDLE io;
    for(i=1;i<argc;++i) {
        if(!wcscmp(argv[i],L"--directory") && i+1<argc) { if(wcslen(argv[i+1])>=2048) return 2; wcscpy_s(directory,2048,argv[++i]); }
        else if(!wcscmp(argv[i],L"--role") && i+1<argc) role=(unsigned)_wtoi(argv[++i]);
        else if(!wcscmp(argv[i],L"--lifetime-ms") && i+1<argc) lifetime=(unsigned)_wtoi(argv[++i]);
        else { fwprintf(stderr,L"grouping_fixture --directory NEW_LOCAL_DIR [--lifetime-ms 300000]\n"); return 2; }
    }
    if(!directory[0] || lifetime<1000 || lifetime>3600000 || role>3) return 2;
    if(!CreateDirectoryW(directory,NULL) && GetLastError()!=ERROR_ALREADY_EXISTS) return 1;
    GetModuleFileNameW(NULL,exe,32768);
    GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user);
    swprintf_s(file,4096,L"%s\\ready-%u-%lu.txt",directory,role,(unsigned long)GetCurrentProcessId());
    { FILE *f=NULL; _wfopen_s(&f,file,L"wb"); if(!f) return 1; fprintf(f,"pid=%lu role=%u creation=%llu\n",(unsigned long)GetCurrentProcessId(),role,(unsigned long long)(((uint64_t)created.dwHighDateTime<<32)|created.dwLowDateTime)); fclose(f); }
    if(role==0 || role==1) {
        unsigned child;
        for(child=role==0?1u:3u;child<=(role==0?2u:3u);++child) {
            STARTUPINFOW startup; PROCESS_INFORMATION process;
            memset(&startup,0,sizeof(startup)); startup.cb=sizeof(startup); memset(&process,0,sizeof(process));
            swprintf_s(command,32768,L"\"%s\" --directory \"%s\" --role %u --lifetime-ms %u",exe,directory,child,lifetime);
            if(!CreateProcessW(exe,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) return 1;
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
        }
    }
    memory=VirtualAlloc(NULL,4*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(memory) memset(memory,(int)role+1,4*1024*1024);
    swprintf_s(file,4096,L"%s\\io-%u-%lu.bin",directory,role,(unsigned long)GetCurrentProcessId());
    io=CreateFileW(file,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,NULL);
    printf("fixture pid=%lu role=%u directory=%ls lifetime_ms=%u\n",(unsigned long)GetCurrentProcessId(),role,directory,lifetime); fflush(stdout);
    start=GetTickCount64();
    while(GetTickCount64()-start<lifetime) {
        swprintf_s(marker,4096,L"%s\\stop-all",directory); if(GetFileAttributesW(marker)!=INVALID_FILE_ATTRIBUTES) break;
        if(role==0) { swprintf_s(marker,4096,L"%s\\exit-root",directory); if(GetFileAttributesW(marker)!=INVALID_FILE_ATTRIBUTES) break; }
        if(io!=INVALID_HANDLE_VALUE && memory) { DWORD bytes; SetFilePointer(io,0,NULL,FILE_BEGIN); WriteFile(io,memory,4096,&bytes,NULL); SetFilePointer(io,0,NULL,FILE_BEGIN); ReadFile(io,memory,4096,&bytes,NULL); }
        Sleep(50);
    }
    if(io!=INVALID_HANDLE_VALUE) CloseHandle(io);
    if(memory) VirtualFree(memory,0,MEM_RELEASE);
    return 0;
}
