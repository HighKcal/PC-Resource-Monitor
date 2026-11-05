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
cl /nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /I"%SOURCE%\include" tests\phase2-review\test_public_layout.c /Fo"%OUT%\test_public_layout.obj" /Fe"%OUT%\test_public_layout.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
