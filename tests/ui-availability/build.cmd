@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-access-20261103\ui-tests"
set "LIBOUT=%~2"
if not defined LIBOUT set "LIBOUT=build-native-ui"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude tests\ui-availability\test_availability.c /Fo"%OUT%\test_availability.obj" /Fe"%OUT%\test_availability.exe" "%LIBOUT%\grouping.lib" /link user32.lib
if errorlevel 1 goto failed
cl /nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /c gui\monitor_gui.c /Fo"%OUT%\monitor_gui.obj"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
