@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-grouping"
set "CORE=%~2"
if not defined CORE set "CORE=build-grouping-after-core\monitor.lib"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude"
cl %CFLAGS% tests\review-grouping\test_gui_review.c /Fo"%OUT%\test_gui_review.obj" /Fe"%OUT%\test_gui_review.exe" "%OUT%\review_process_grouping.obj" "%OUT%\review_program_groups.obj" "%OUT%\review_process_grouping_windows.obj" "%CORE%" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
