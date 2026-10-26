@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-monitor"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
set "PFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
cl %PFLAGS% tests\review-monitor\test_mature_probe.c /Fo"%OUT%\test_mature_probe.obj" /Fe"%OUT%\test_mature_probe.exe" "%OUT%\monitor.obj" "%OUT%\monitor_storage.obj" "%OUT%\baseline.obj" "%OUT%\monitor_identity.obj" "%OUT%\resource_core.obj" "%OUT%\resource_collector.obj" "%OUT%\sqlite3.obj" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
