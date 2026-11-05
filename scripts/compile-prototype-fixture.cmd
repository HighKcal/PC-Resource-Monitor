@echo off
rem Synthetic fixture adapter plus matching selected-root product objects.
rem Arguments: output-directory source-root object-prefix. CFLAGS is borrowed.
rem BUILD_OBJECTS is returned; no setlocal, like compile-sources.cmd.
call scripts\build-sources.cmd "%~2"
set "FIXTURE_PRODUCT_SOURCES=%PROTOTYPE_SUPPORT_SOURCES%"
set "FIXTURE_FLAGS="
if exist "%~2\prototype\experiment_internal.h" (
  set "FIXTURE_PRODUCT_SOURCES=%PROTOTYPE_SOURCES%"
  set "FIXTURE_FLAGS=/DEXPERIMENT_MODULE_SPLIT"
)
call scripts\compile-sources.cmd FIXTURE_PRODUCT_SOURCES "%~1" "%~2" "%~3" ""
if errorlevel 1 exit /b 1
set "FIXTURE_PRODUCT_OBJECTS=%BUILD_OBJECTS%"
cl %CFLAGS% %FIXTURE_FLAGS% /I"%~2\prototype" /Itests\support /c tests\support\experiment_fixture.c /Fo"%~1\%~3experiment_fixture.obj"
if errorlevel 1 exit /b 1
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%~1\%~3monitor_fixture.obj"
if errorlevel 1 exit /b 1
set "BUILD_OBJECTS=%FIXTURE_PRODUCT_OBJECTS% "%~1\%~3experiment_fixture.obj" "%~1\%~3monitor_fixture.obj""
exit /b 0
