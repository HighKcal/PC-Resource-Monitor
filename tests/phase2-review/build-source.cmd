@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "CORE=%~2"
if not defined OUT goto failed
if not exist "%CORE%" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Iprototype /Itests\support /Ithird_party\sqlite"
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd PROTOTYPE_SOURCES "%OUT%" . "review_" ""
if errorlevel 1 goto failed
set "PROTOTYPE_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% /Dcalloc=review_source_calloc /Dmalloc=review_source_malloc /Dfree=review_source_free /c storage\monitor_source.c /Fo"%OUT%\review_monitor_source.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\review_monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /DEXPERIMENT_MODULE_SPLIT /c tests\support\experiment_fixture.c /Fo"%OUT%\review_experiment_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase2-review\test_source_contract.c /Fo"%OUT%\test_source_contract.obj" /Fe"%OUT%\test_source_contract.exe" "%OUT%\review_monitor_source.obj" "%OUT%\review_monitor_fixture.obj" "%OUT%\review_experiment_fixture.obj" %PROTOTYPE_OBJECTS% "%CORE%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
