@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-grouping"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude"
cl %CFLAGS% /c grouping\process_grouping.c /Fo"%OUT%\review_process_grouping.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c grouping\program_groups.c /Fo"%OUT%\review_program_groups.obj"
if errorlevel 1 goto failed
cl %CFLAGS% /c grouping\process_grouping_windows.c /Fo"%OUT%\review_process_grouping_windows.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\review-grouping\test_program_groups_review.c /Fo"%OUT%\test_program_groups_review.obj" /Fe"%OUT%\test_program_groups_review.exe" "%OUT%\review_process_grouping.obj" "%OUT%\review_program_groups.obj" "%OUT%\review_process_grouping_windows.obj"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
