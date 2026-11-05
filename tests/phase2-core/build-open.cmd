@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "PRODUCT=%~2"
if not defined OUT goto failed
if not exist "%PRODUCT%\monitor.lib" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
cl %CFLAGS% /Dcalloc=review_engine_calloc /Dfree=review_engine_free /c engine\monitor.c /Fo"%OUT%\injected_monitor.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /Dcalloc=review_store_calloc /c storage\monitor_storage.c /Fo"%OUT%\injected_store.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase2-core\test_store_open.c /Fo"%OUT%\test_store_open.obj" /Fe"%OUT%\test_store_open.exe" "%OUT%\injected_monitor.obj" "%OUT%\injected_store.obj" "%OUT%\monitor_fixture.obj" "%PRODUCT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
