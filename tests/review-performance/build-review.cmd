@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-review-performance"
set "PRODUCT=%~2"
if not defined PRODUCT set "PRODUCT=build-monitor"
set "SOURCE=%~3"
if not defined SOURCE set "SOURCE=."
set "INJECT=%~4"
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /I"%SOURCE%\include" /I"%SOURCE%\collector" /I"%SOURCE%\engine" /I"%SOURCE%\storage" /I"%SOURCE%\detection" /I"%SOURCE%\third_party\sqlite""
call scripts\build-sources.cmd "%SOURCE%"
set "INJECTED_OBJECT="
if /I "%INJECT%"=="inject" (
  call scripts\compile-sources.cmd BASELINE_SOURCES "%OUT%" "%SOURCE%" "injected_" "/Dcalloc=review_baseline_calloc /Dmalloc=review_baseline_malloc /Drealloc=review_baseline_realloc"
  if errorlevel 1 goto failed
  call set "INJECTED_OBJECT=%%BUILD_OBJECTS%%"
)
cl %CFLAGS% /c tests\support\monitor_fixture.c /Fo"%OUT%\monitor_fixture.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\review-performance\test_baseline_oracle.c "%OUT%\monitor_fixture.obj" /Fo"%OUT%\test_baseline_oracle.obj" /Fe"%OUT%\test_baseline_oracle.exe" %INJECTED_OBJECT% "%PRODUCT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
