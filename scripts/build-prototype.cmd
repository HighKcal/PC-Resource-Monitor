@echo off
setlocal
pushd "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=build-prototype"
set "MONITOR_OUT=%~2"
if not defined MONITOR_OUT set "MONITOR_OUT=build-monitor"
if not exist "%MONITOR_OUT%\monitor.lib" (
  echo Build the existing monitor library first: build-monitor.cmd %MONITOR_OUT%
  goto failed
)
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd PROTOTYPE_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
set "PROTOTYPE_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% app\experiment_cli.c /Fo"%OUT%\experiment_cli.obj" /Fe"%OUT%\prototype_cli.exe" %PROTOTYPE_OBJECTS% "%MONITOR_OUT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% gui\experiment_gui.c /Fo"%OUT%\experiment_gui.obj" /Fe"%OUT%\prototype_gui.exe" %PROTOTYPE_OBJECTS% "%MONITOR_OUT%\monitor.lib" /link user32.lib comctl32.lib gdi32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\prototype\test_experiment.c /Fo"%OUT%\test_experiment.obj" /Fe"%OUT%\test_experiment.exe" %PROTOTYPE_OBJECTS% "%MONITOR_OUT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
echo Prototype build succeeded: %OUT%
popd
exit /b 0
:failed
popd
exit /b 1
