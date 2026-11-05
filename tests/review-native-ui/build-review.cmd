@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-native-ui"
set "CORE=%~2"
if not defined CORE set "CORE=build-native-ui\monitor.lib"
set "TESTSRC=%~3"
if not defined TESTSRC set "TESTSRC=tests\review-native-ui\test_native_ui_review.c"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude"
for %%f in (grouping\process_grouping grouping\program_groups grouping\process_grouping_windows) do (
  cl %CFLAGS% /c %%f.c /Fo"%OUT%\review_%%~nf.obj"
  if errorlevel 1 goto failed
)
cl %CFLAGS% "%TESTSRC%" /Fo"%OUT%\test_native_ui_review.obj" /Fe"%OUT%\test_native_ui_review.exe" "%OUT%\review_process_grouping.obj" "%OUT%\review_program_groups.obj" "%OUT%\review_process_grouping_windows.obj" "%CORE%" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
