@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
set "SOURCE=%~2"
set "PRODUCT=%~3"
if not defined PRODUCT goto failed
if exist "%OUT%" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%SOURCE%\include" /I"%SOURCE%\collector" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\third_party\sqlite""
call scripts\build-sources.cmd "%SOURCE%"
call scripts\compile-sources.cmd MONITOR_STATE_SOURCES "%OUT%" "%SOURCE%" "injected_" "/Dcalloc=review_calloc"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase1-build\test_calloc_hits.c /Fo"%OUT%\test_calloc_hits.obj" /Fe"%OUT%\test_calloc_hits.exe" %BUILD_OBJECTS% "%PRODUCT%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
