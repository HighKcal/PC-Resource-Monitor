@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "CORE=%~2"
if not defined OUT goto failed
if not exist "%CORE%" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Itests\support /Ithird_party\sqlite"
cl %CFLAGS% /Dcalloc=review_history_calloc /c detection\baseline_history.c /Fo"%OUT%\review_baseline_history.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\review_monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase2-review\test_history_scope.c /Fo"%OUT%\test_history_scope.obj" /Fe"%OUT%\test_history_scope.exe" "%OUT%\review_baseline_history.obj" "%OUT%\review_monitor_fixture.obj" "%CORE%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
