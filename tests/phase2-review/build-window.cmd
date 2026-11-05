@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
set "SOURCE=%~2"
if not defined OUT goto failed
if not defined SOURCE set "SOURCE=."
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /I"%SOURCE%\include" /I"%SOURCE%\engine" /I"%SOURCE%\detection""
cl %CFLAGS% /c "%SOURCE%\engine\monitor_window.c" /Fo"%OUT%\monitor_window.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase2-review\test_window_standalone.c /Fo"%OUT%\test_window_standalone.obj" /Fe"%OUT%\test_window_standalone.exe" "%OUT%\monitor_window.obj"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
