@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
set "SOURCE=%~2"
if not defined SOURCE set "SOURCE=."
set "PRODUCT=%~3"
if not defined PRODUCT set "PRODUCT=build-monitor\monitor.lib"
if not defined OUT set "OUT=build-phase1-20261104\retention"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%SOURCE%\include" /I"%SOURCE%\collector" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\third_party\sqlite" /I"%SOURCE%\prototype""
call scripts\compile-prototype-fixture.cmd "%OUT%" "%SOURCE%" ""
if errorlevel 1 goto failed
set "SUPPORT_OBJECTS=%BUILD_OBJECTS%"
rem Current product objects or historical monolith are selected by the adapter.
cl %CFLAGS% tests\phase1-retention\test_retention.c /Fo"%OUT%\test_retention.obj" /Fe"%OUT%\test_retention.exe" %SUPPORT_OBJECTS% "%PRODUCT%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
