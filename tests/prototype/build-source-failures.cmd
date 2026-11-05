@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "MONITOR_LIB=%~2"
if not defined OUT goto failed
if not exist "%MONITOR_LIB%" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Iprototype /Ithird_party\sqlite /Itests\support"
call scripts\build-sources.cmd .
set "PROTOTYPE_OBJECTS="
for %%f in (%PROTOTYPE_SOURCES%) do (
  call :compile "%%f" "%%~nf"
  if errorlevel 1 goto failed
)
cl %CFLAGS% /Dcalloc=prototype_test_source_calloc /Dfree=prototype_test_source_free /c storage\monitor_source.c /Fo"%OUT%\source_injected.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /DEXPERIMENT_MODULE_SPLIT /c tests\support\experiment_fixture.c /Fo"%OUT%\experiment_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\prototype\test_source_failures.c /Fo"%OUT%\test_source_failures.obj" /Fe"%OUT%\test_source_failures.exe" %PROTOTYPE_OBJECTS% "%OUT%\source_injected.obj" "%OUT%\experiment_fixture.obj" "%OUT%\monitor_fixture.obj" "%MONITOR_LIB%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:compile
set "INJECTION="
if "%~2"=="experiment" set "INJECTION=/Dmonitor_source_read_committed=prototype_test_source_read"
cl %CFLAGS% %INJECTION% /c "%~1.c" /Fo"%OUT%\%~2.obj"
if errorlevel 1 exit /b 1
set "PROTOTYPE_OBJECTS=%PROTOTYPE_OBJECTS% "%OUT%\%~2.obj""
exit /b 0
:failed
popd
exit /b 1
