@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-access-20261103\focus-anchor"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
cl /nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DUNICODE /D_UNICODE tests\access-coverage\focus_anchor.c /Fo"%OUT%\focus_anchor.obj" /Fe"%OUT%\focus_anchor.exe" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
