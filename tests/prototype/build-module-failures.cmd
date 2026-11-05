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
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Iprototype /Ithird_party\sqlite"
call scripts\build-sources.cmd .
set "INJECTED_OBJECTS="
for %%f in (%PROTOTYPE_SOURCES%) do (
  call :compile "%%f" "%%~nf"
  if errorlevel 1 goto failed
)
cl %CFLAGS% /DEXPERIMENT_MODULE_SPLIT /Itests\support /c tests\support\experiment_fixture.c /Fo"%OUT%\experiment_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\prototype\test_module_failures.c /Fo"%OUT%\test_module_failures.obj" /Fe"%OUT%\test_module_failures.exe" %INJECTED_OBJECTS% "%OUT%\experiment_fixture.obj" "%OUT%\monitor_fixture.obj" "%MONITOR_LIB%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:compile
set "ALLOCATOR_TAG="
if "%~2"=="experiment" set "ALLOCATOR_TAG=coordinator"
if "%~2"=="experiment_store" set "ALLOCATOR_TAG=store"
if "%~2"=="experiment_history" set "ALLOCATOR_TAG=history"
if "%~2"=="experiment_replay" set "ALLOCATOR_TAG=replay"
set "INJECTION="
if defined ALLOCATOR_TAG set "INJECTION=/Dmalloc=prototype_%ALLOCATOR_TAG%_malloc /Dcalloc=prototype_%ALLOCATOR_TAG%_calloc"
cl %CFLAGS% %INJECTION% /c "%~1.c" /Fo"%OUT%\%~2.obj"
if errorlevel 1 exit /b 1
set "INJECTED_OBJECTS=%INJECTED_OBJECTS% "%OUT%\%~2.obj""
exit /b 0
:failed
popd
exit /b 1
