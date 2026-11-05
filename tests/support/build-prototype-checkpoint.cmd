@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "CHECKPOINT=%~2"
set "MONITOR_LIB=%~3"
if not defined OUT goto failed
if not exist "%CHECKPOINT%\experiment.c" goto failed
if not exist "%MONITOR_LIB%" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
call scripts\build-sources.cmd . "%CHECKPOINT%\experiment.c"
call scripts\compile-sources.cmd PROTOTYPE_MODULE_SOURCES "%OUT%" "%CHECKPOINT%" "" ""
if errorlevel 1 goto failed
set "CHECKPOINT_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% app\experiment_cli.c /Fo"%OUT%\experiment_cli.obj" /Fe"%OUT%\prototype_cli.exe" %CHECKPOINT_OBJECTS% "%MONITOR_LIB%" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% gui\experiment_gui.c /Fo"%OUT%\experiment_gui.obj" /Fe"%OUT%\prototype_gui.exe" %CHECKPOINT_OBJECTS% "%MONITOR_LIB%" /link user32.lib comctl32.lib gdi32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\prototype\test_experiment.c /Fo"%OUT%\test_experiment.obj" /Fe"%OUT%\test_experiment.exe" %CHECKPOINT_OBJECTS% "%MONITOR_LIB%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
