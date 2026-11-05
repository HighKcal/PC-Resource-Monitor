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
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd MONITOR_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
lib /nologo /OUT:"%OUT%\monitor.lib" %BUILD_OBJECTS% "%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
call scripts\compile-sources.cmd GROUPING_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
lib /nologo /OUT:"%OUT%\grouping.lib" %BUILD_OBJECTS%
if errorlevel 1 goto failed
rem Suppress the default asInvoker fragment; embed the GUI-only source manifest.
cl %CFLAGS% gui\monitor_gui.c /Fo"%OUT%\monitor_gui.obj" /Fe"%OUT%\monitor_gui.exe" "%OUT%\monitor.lib" "%OUT%\grouping.lib" /link /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:gui\monitor_gui.manifest user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
rem Keep any sidecar from an earlier build identical to the embedded policy.
mt.exe -nologo -inputresource:"%OUT%\monitor_gui.exe";#1 -out:"%OUT%\monitor_gui.exe.manifest"
if errorlevel 1 goto failed
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\integration\test_monitor.c "%OUT%\monitor_fixture.obj" /Fo"%OUT%\test_monitor.obj" /Fe"%OUT%\test_monitor.exe" "%OUT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\grouping\test_grouping.c /Fo"%OUT%\test_grouping.obj" /Fe"%OUT%\test_grouping.exe" "%OUT%\grouping.lib" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\grouping\grouping_fixture.c /Fo"%OUT%\grouping_fixture.obj" /Fe"%OUT%\grouping_fixture.exe" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\grouping\replay_grouping.c /Fo"%OUT%\replay_grouping.obj" /Fe"%OUT%\replay_grouping.exe" "%OUT%\grouping.lib" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\grouping\probe_fixture.c /Fo"%OUT%\probe_fixture.obj" /Fe"%OUT%\probe_fixture.exe" "%OUT%\grouping.lib" "%OUT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
echo Monitor build succeeded: %OUT%
popd
exit /b 0
:failed
popd
exit /b 1
