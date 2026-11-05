@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-access-coverage"
set "CORE=%~2"
if not defined CORE set "CORE=build-access-20261103\after\monitor.lib"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude"
for %%f in (grouping\process_grouping grouping\program_groups grouping\process_grouping_windows) do (
  cl %CFLAGS% /c %%f.c /Fo"%OUT%\review_%%~nf.obj"
  if errorlevel 1 goto failed
)
for %%t in (test_native_ui_review test_gui_review) do (
  cl %CFLAGS% "tests\review-access-coverage\%%t.c" /Fo"%OUT%\%%t.obj" /Fe"%OUT%\%%t.exe" "%OUT%\review_process_grouping.obj" "%OUT%\review_program_groups.obj" "%OUT%\review_process_grouping_windows.obj" "%CORE%" /link user32.lib comctl32.lib shell32.lib gdi32.lib
  if errorlevel 1 goto failed
)
popd
exit /b 0
:failed
popd
exit /b 1
