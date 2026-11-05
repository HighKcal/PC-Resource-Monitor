@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "CORE=%~2"
set "SOURCE=%~3"
if not defined SOURCE set "SOURCE=."
if not defined OUT goto failed
if not defined CORE goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%SOURCE%\include" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\prototype" /I"%SOURCE%\third_party\sqlite""
call scripts\compile-prototype-fixture.cmd "%OUT%" "%SOURCE%" "review_"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase1-review\test_retention_episode_backlog.c /Fo"%OUT%\test_retention_episode_backlog.obj" /Fe"%OUT%\test_retention_episode_backlog.exe" %BUILD_OBJECTS% "%CORE%" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
