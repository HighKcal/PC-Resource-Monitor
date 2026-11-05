@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-prototype-final"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector /Iengine /Istorage /Idetection /Ithird_party\sqlite"
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd PROTOTYPE_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
set "PROTOTYPE_OBJECTS=%BUILD_OBJECTS%"
set "REVIEW_OBJECTS="
for %%f in (%MONITOR_SOURCES%) do call set "REVIEW_OBJECTS=%%REVIEW_OBJECTS%% "%OUT%\%%~nf.obj""
cl %CFLAGS% tests\review-prototype\test_independent.c /Fo"%OUT%\test_independent.obj" /Fe"%OUT%\test_independent.exe" %PROTOTYPE_OBJECTS% %REVIEW_OBJECTS% "%OUT%\sqlite3.obj" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
