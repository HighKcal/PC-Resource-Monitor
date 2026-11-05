@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
set "SOURCE=%~2"
set "MONITOR_LIB=%~3"
if not defined OUT goto failed
if not defined SOURCE goto failed
if not exist "%MONITOR_LIB%" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%SOURCE%" /I"%SOURCE%\include" /I"%SOURCE%\collector" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\third_party\sqlite""
call scripts\compile-prototype-fixture.cmd "%OUT%" "%SOURCE%" ""
if errorlevel 1 goto failed
rem Current product objects or historical monolith are selected by the adapter.
cl %CFLAGS% tests\phase1-main\retained_probe.c /Fo"%OUT%\retained_probe.obj" /Fe"%OUT%\retained_probe.exe" %BUILD_OBJECTS% "%MONITOR_LIB%" /link user32.lib psapi.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
