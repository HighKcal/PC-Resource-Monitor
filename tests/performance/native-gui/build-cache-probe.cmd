@echo off
setlocal
pushd "%~dp0..\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-gui-perf-20261103\cache-probe"
rem Library directory and source root must explicitly describe the same build.
set "PRODUCT=%~2"
if not defined PRODUCT (
  echo Matching PRODUCT directory and SOURCE root are required. 1>&2
  echo Usage: build-cache-probe.cmd OUTPUT PRODUCT SOURCE 1>&2
  goto failed
)
set "SOURCE=%~3"
if not defined SOURCE (
  echo Matching PRODUCT directory and SOURCE root are required. 1>&2
  echo Usage: build-cache-probe.cmd OUTPUT PRODUCT SOURCE 1>&2
  goto failed
)
if not exist "%PRODUCT%\monitor.lib" goto failed
if not exist "%PRODUCT%\grouping.lib" goto failed
if not exist "%SOURCE%\include\monitor.h" goto failed
if not exist "%OUT%\monitor_gui.c" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /DGUI_PERF_COUNTERS=0 /I"%SOURCE%\include" /I"%SOURCE%\collector" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\third_party\sqlite" /Itests\support"
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% "%OUT%\monitor_gui.c" "%OUT%\monitor_fixture.obj" /Fo"%OUT%\cache_probe.obj" /Fe"%OUT%\monitor_gui_cache_probe.exe" "%PRODUCT%\monitor.lib" "%PRODUCT%\grouping.lib" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
