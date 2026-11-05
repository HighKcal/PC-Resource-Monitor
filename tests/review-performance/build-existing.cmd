@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-performance-existing"
set "PRODUCT=%~2"
if not defined PRODUCT set "PRODUCT=build-perf-20261103\after-monitor"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd MONITOR_STATE_SOURCES "%OUT%" . "injected_" "/Dcalloc=review_calloc"
if errorlevel 1 goto failed
set "INJECTED_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% tests\review-monitor\test_independent.c /Fo"%OUT%\test_monitor_review.obj" /Fe"%OUT%\test_monitor_review.exe" %INJECTED_OBJECTS% "%PRODUCT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
call scripts\compile-sources.cmd PROTOTYPE_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
set "PROTOTYPE_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% tests\review-prototype\test_independent.c /Fo"%OUT%\test_prototype_review.obj" /Fe"%OUT%\test_prototype_review.exe" %PROTOTYPE_OBJECTS% "%PRODUCT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
