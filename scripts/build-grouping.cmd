@echo off
setlocal
pushd "%~dp0.."
set "OUT=%~1"
if not defined OUT set "OUT=build-monitor"
if not exist "%OUT%\monitor.lib" goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /D_WIN32_WINNT=0x0601 /DPSAPI_VERSION=2 /DUNICODE /D_UNICODE /Iinclude"
call scripts\build-sources.cmd .
call scripts\compile-sources.cmd GROUPING_SOURCES "%OUT%" . "" ""
if errorlevel 1 goto failed
lib /nologo /OUT:"%OUT%\grouping.lib" %BUILD_OBJECTS%
if errorlevel 1 goto failed
rem Keep the same GUI-only execution policy when relinking grouping changes.
cl %CFLAGS% gui\monitor_gui.c /Fo"%OUT%\monitor_gui.obj" /Fe"%OUT%\monitor_gui.exe" "%OUT%\monitor.lib" "%OUT%\grouping.lib" /link /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:gui\monitor_gui.manifest user32.lib comctl32.lib shell32.lib gdi32.lib
if errorlevel 1 goto failed
rem Keep any sidecar from an earlier build identical to the embedded policy.
mt.exe -nologo -inputresource:"%OUT%\monitor_gui.exe";#1 -out:"%OUT%\monitor_gui.exe.manifest"
if errorlevel 1 goto failed
for %%f in (test_grouping replay_grouping) do (
  cl %CFLAGS% tests\grouping\%%f.c /Fo"%OUT%\%%f.obj" /Fe"%OUT%\%%f.exe" "%OUT%\grouping.lib" /link user32.lib
  if errorlevel 1 goto failed
)
cl %CFLAGS% tests\grouping\grouping_fixture.c /Fo"%OUT%\grouping_fixture.obj" /Fe"%OUT%\grouping_fixture.exe" /link user32.lib
if errorlevel 1 goto failed
cl %CFLAGS% tests\grouping\probe_fixture.c /Fo"%OUT%\probe_fixture.obj" /Fe"%OUT%\probe_fixture.exe" "%OUT%\grouping.lib" "%OUT%\monitor.lib" /link user32.lib
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
