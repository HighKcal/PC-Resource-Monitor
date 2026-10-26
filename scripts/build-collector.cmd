@echo off
setlocal
pushd "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=build"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Icollector"
cl %CFLAGS% /c collector\resource_core.c /Fo"%OUT%\resource_core.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c collector\resource_collector.c /Fo"%OUT%\resource_collector.obj"
if errorlevel 1 goto failed
lib /nologo /OUT:"%OUT%\resource_collector.lib" "%OUT%\resource_core.obj" "%OUT%\resource_collector.obj"
if errorlevel 1 goto failed
cl %CFLAGS% app\console.c /Fo"%OUT%\console.obj" /Fe"%OUT%\resource_console.exe" "%OUT%\resource_collector.lib" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\collector\test_core.c /Fo"%OUT%\test_core.obj" /Fe"%OUT%\test_core.exe" "%OUT%\resource_core.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\collector\test_windows.c /Fo"%OUT%\test_windows.obj" /Fe"%OUT%\test_windows.exe" "%OUT%\resource_collector.lib" /link user32.lib
if errorlevel 1 goto failed
echo Collector build succeeded: %OUT%
popd
exit /b 0
:failed
popd
exit /b 1
