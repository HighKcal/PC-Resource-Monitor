@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-admin-gui-20261103\review"
set "CORE=%~2"
if not defined CORE set "CORE=build-admin-gui-20261103\admin\monitor.lib"
set "GROUP=%~3"
if not defined GROUP set "GROUP=build-admin-gui-20261103\admin\grouping.lib"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude tests\review-admin-gui\test_admin_gui.c /Fo"%OUT%\test_admin_gui.obj" /Fe"%OUT%\test_admin_gui.exe" "%CORE%" "%GROUP%" /link /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
