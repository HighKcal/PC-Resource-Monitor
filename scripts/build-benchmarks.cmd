@echo off
setlocal
pushd "%~dp0.."
set "BENCH_LIB=%~1"
if not defined BENCH_LIB set "BENCH_LIB=build-monitor\monitor.lib"
set "OUT=%~2"
if not defined OUT set "OUT=build-monitor"
set "BENCH_SOURCE_ROOT=%~3"
if not defined BENCH_SOURCE_ROOT set "BENCH_SOURCE_ROOT=."
if not exist "%BENCH_LIB%" (
  echo Build the product library first with build-monitor.cmd.
  goto failed
)
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%BENCH_SOURCE_ROOT%\include" /I"%BENCH_SOURCE_ROOT%\collector" /I"%BENCH_SOURCE_ROOT%\engine" /I"%BENCH_SOURCE_ROOT%\storage" /I"%BENCH_SOURCE_ROOT%\detection" /I"%BENCH_SOURCE_ROOT%\third_party\sqlite" /I"%BENCH_SOURCE_ROOT%\prototype""
set "BENCH_TARGETS=benchmark_monitor_batch benchmark_monitor_query"
if exist "%BENCH_SOURCE_ROOT%\prototype\experiment.c" (
  set "BENCH_TARGETS=%BENCH_TARGETS% benchmark_pipeline"
) else (
  echo Optional pipeline benchmark skipped: prototype\experiment.c absent from %BENCH_SOURCE_ROOT%.
)
rem The fixture adapter alone may include a historical monolith. Current roots
rem link the independent prototype objects, all from the selected source root.
set "BENCH_SUPPORT_OBJECTS="
if exist "%BENCH_SOURCE_ROOT%\prototype\experiment.c" (
  call scripts\compile-prototype-fixture.cmd "%OUT%" "%BENCH_SOURCE_ROOT%" "bench_"
  if errorlevel 1 goto failed
  call set "BENCH_SUPPORT_OBJECTS=%%BUILD_OBJECTS%%"
)
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
for %%f in (%BENCH_TARGETS%) do (
  if "%%f"=="benchmark_pipeline" (
    cl %CFLAGS% tests\performance\%%f.c /Fo"%OUT%\%%f.obj" /Fe"%OUT%\%%f.exe" %BENCH_SUPPORT_OBJECTS% "%BENCH_LIB%" /link user32.lib psapi.lib
  ) else (
    cl %CFLAGS% tests\performance\%%f.c "%OUT%\monitor_fixture.obj" /Fo"%OUT%\%%f.obj" /Fe"%OUT%\%%f.exe" "%BENCH_LIB%" /link user32.lib psapi.lib
  )
  if errorlevel 1 goto failed
)
echo Benchmarks built against %BENCH_LIB%: %OUT%
popd
exit /b 0
:failed
popd
exit /b 1
