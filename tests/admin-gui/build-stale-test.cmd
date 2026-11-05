@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-admin-gui-20261103\stale-tests"
set "LIBOUT=%~2"
if not defined LIBOUT set "LIBOUT=build-admin-gui-20261103\final"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude tests\admin-gui\test_stale_status.c /Fo"%OUT%\test_stale_status.obj" /Fe"%OUT%\test_stale_status.exe" "%LIBOUT%\monitor.lib" "%LIBOUT%\grouping.lib" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
