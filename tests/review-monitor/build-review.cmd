@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-monitor"
set "TESTSRC=%~2"
if not defined TESTSRC set "TESTSRC=tests\review-monitor\test_independent.c"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "RFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
set "CFLAGS=%RFLAGS%"
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd MONITOR_STATE_SOURCES "%OUT%" . "" "/Dcalloc=review_calloc"
if errorlevel 1 goto failed
set "REVIEW_OBJECTS=%BUILD_OBJECTS%"
set "REVIEW_NORMAL_SOURCES=%BASELINE_SOURCES% %COLLECTOR_SOURCES%"
call scripts\compile-sources.cmd REVIEW_NORMAL_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
set "REVIEW_OBJECTS=%REVIEW_OBJECTS% %BUILD_OBJECTS%"
cl /nologo /std:c17 /O2 /MT /w /DSQLITE_THREADSAFE=1 /DSQLITE_DQS=0 /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_ENABLE_DBSTAT_VTAB /c third_party\sqlite\sqlite3.c /Fo"%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
cl %RFLAGS% "%TESTSRC%" /Fo"%OUT%\test_independent.obj" /Fe"%OUT%\test_independent.exe" %REVIEW_OBJECTS% "%OUT%\sqlite3.obj" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
