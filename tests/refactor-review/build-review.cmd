@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
set "SOURCE=%~2"
set "PRODUCT=%~3"
set "PROTOTYPE=%~4"
if not defined PROTOTYPE goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%SOURCE%\include" /I"%SOURCE%\collector" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\third_party\sqlite""
call scripts\build-sources.cmd "%SOURCE%"
call scripts\compile-sources.cmd MONITOR_STATE_SOURCES "%OUT%" "%SOURCE%" "injected_" "/Dmalloc=review_malloc /Dfree=review_free"
if errorlevel 1 goto failed
set "INJECTED_OBJECTS=%BUILD_OBJECTS%"
set "PROTOTYPE_OBJECTS="
for %%f in (%PROTOTYPE_SOURCES%) do call set "PROTOTYPE_OBJECTS=%%PROTOTYPE_OBJECTS%% "%PROTOTYPE%\%%~nf.obj""
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\refactor-review\test_failure_boundaries.c "%OUT%\monitor_fixture.obj" /Fo"%OUT%\test_failure_boundaries.obj" /Fe"%OUT%\test_failure_boundaries.exe" %INJECTED_OBJECTS% %PROTOTYPE_OBJECTS% "%PRODUCT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
