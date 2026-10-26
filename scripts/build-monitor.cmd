@echo off
setlocal
pushd "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=build-monitor"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
rem Vendor warnings are isolated from product /W4 /WX. Source is pinned 3.54.1.
cl /nologo /std:c17 /O2 /MT /w /DSQLITE_THREADSAFE=1 /DSQLITE_DQS=0 /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_ENABLE_DBSTAT_VTAB /c third_party\sqlite\sqlite3.c /Fo"%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
for %%f in (collector\resource_core collector\resource_collector collector\monitor_identity engine\monitor storage\monitor_storage detection\baseline) do (
  cl %CFLAGS% /c %%f.c /Fo"%OUT%\%%~nf.obj"
  if errorlevel 1 goto failed
)
lib /nologo /OUT:"%OUT%\monitor.lib" "%OUT%\monitor.obj" "%OUT%\monitor_storage.obj" "%OUT%\baseline.obj" "%OUT%\monitor_identity.obj" "%OUT%\resource_core.obj" "%OUT%\resource_collector.obj" "%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
cl %CFLAGS% gui\monitor_gui.c /Fo"%OUT%\monitor_gui.obj" /Fe"%OUT%\monitor_gui.exe" "%OUT%\monitor.lib" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\integration\test_monitor.c /Fo"%OUT%\test_monitor.obj" /Fe"%OUT%\test_monitor.exe" "%OUT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
echo Monitor build succeeded: %OUT%
popd
exit /b 0
:failed
popd
exit /b 1
