@echo off
setlocal
pushd "%~dp0.."
set "BENCH_LIB=%~1"
if not defined BENCH_LIB set "BENCH_LIB=build-monitor\monitor.lib"
set "OUT=%~2"
if not defined OUT set "OUT=build-monitor"
if not exist "%BENCH_LIB%" (
  echo Build the product library first with build-monitor.cmd.
  goto failed
)
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
for %%f in (benchmark_monitor_batch benchmark_monitor_query) do (
  cl %CFLAGS% tests\performance\%%f.c /Fo"%OUT%\%%f.obj" /Fe"%OUT%\%%f.exe" "%BENCH_LIB%" /link user32.lib
  if errorlevel 1 goto failed
)
echo Benchmarks built against %BENCH_LIB%: %OUT%
popd
exit /b 0
:failed
popd
exit /b 1
