@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-prototype"
set "BACKEND=%~2"
if not defined BACKEND set "BACKEND=prototype\experiment.c"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
cl /nologo /std:c17 /O2 /MT /w /DSQLITE_THREADSAFE=1 /DSQLITE_DQS=0 /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_ENABLE_DBSTAT_VTAB /c third_party\sqlite\sqlite3.c /Fo"%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd MONITOR_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
set "REVIEW_OBJECTS=%BUILD_OBJECTS%"
rem BACKEND remains a supported historical override. Its companions use its folder.
call scripts\build-sources.cmd . "%BACKEND%"
call scripts\compile-sources.cmd PROTOTYPE_MODULE_SOURCES "%OUT%" "%PROTOTYPE_FOLDER%" "" ""
if errorlevel 1 goto failed
set "PROTOTYPE_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% tests\review-prototype\test_independent.c /Fo"%OUT%\test_independent.obj" /Fe"%OUT%\test_independent.exe" %PROTOTYPE_OBJECTS% %REVIEW_OBJECTS% "%OUT%\sqlite3.obj" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
