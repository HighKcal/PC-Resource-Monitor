@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-monitor"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "RFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
rem Allocation injection follows functions moved from monitor.c into storage.
for %%f in (engine\monitor storage\monitor_storage) do (
  cl %RFLAGS% /Dcalloc=review_calloc /c %%f.c /Fo"%OUT%\%%~nf.obj"
  if errorlevel 1 goto failed
)
for %%f in (detection\baseline collector\monitor_identity collector\resource_core collector\resource_collector) do (
  cl %RFLAGS% /c %%f.c /Fo"%OUT%\%%~nf.obj"
  if errorlevel 1 goto failed
)
cl /nologo /std:c17 /O2 /MT /w /DSQLITE_THREADSAFE=1 /DSQLITE_DQS=0 /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_ENABLE_DBSTAT_VTAB /c third_party\sqlite\sqlite3.c /Fo"%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
cl %RFLAGS% tests\review-monitor\test_independent.c /Fo"%OUT%\test_independent.obj" /Fe"%OUT%\test_independent.exe" "%OUT%\monitor.obj" "%OUT%\monitor_storage.obj" "%OUT%\baseline.obj" "%OUT%\monitor_identity.obj" "%OUT%\sqlite3.obj" "%OUT%\resource_core.obj" "%OUT%\resource_collector.obj" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
