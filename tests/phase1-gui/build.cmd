@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-phase1-20261104\gui-only"
set "CORE=%~2"
if not defined CORE set "CORE=build-refactor-20261104\after-monitor\monitor.lib"
set "GROUP=%~3"
if not defined GROUP set "GROUP=build-refactor-20261104\after-monitor\grouping.lib"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude /Ithird_party\sqlite"
cl %CFLAGS% tests\phase1-gui\test_feedback.c /Fo"%OUT%\test_feedback.obj" /Fe"%OUT%\test_feedback.exe" "%CORE%" "%GROUP%" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
if /I "%~4"=="historical-before" (
  rem Explicit historical reproduction needs the separately preserved phase-start GUI.
  cl %CFLAGS% /DPHASE1_BEFORE tests\phase1-gui\test_feedback.c /Fo"%OUT%\test_feedback_before.obj" /Fe"%OUT%\test_feedback_before.exe" "%CORE%" "%GROUP%" /link user32.lib comctl32.lib shell32.lib gdi32.lib
  if errorlevel 1 goto failed
)
cl %CFLAGS% tests\phase1-gui\test_feedback_db.c /Fo"%OUT%\test_feedback_db.obj" /Fe"%OUT%\test_feedback_db.exe" "%CORE%" "%GROUP%" /link user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
