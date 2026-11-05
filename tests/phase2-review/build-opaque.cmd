@echo off
setlocal
pushd "%~dp0..\.."
set "OUT=%~1"
if not defined OUT goto failed
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /Iinclude /Iengine /Istorage /Idetection"
for %%h in (PUBLIC STORE SOURCE BASELINE) do (
  call :normal %%h
  if errorlevel 1 goto failed
  for %%p in (ENGINE_LAYOUT STORE_LAYOUT INSTANCE_LAYOUT SQLITE_LAYOUT) do (
    call :reject %%h %%p
    if errorlevel 1 goto failed
  )
)
echo OPAQUE_RESULT normal_headers=4 rejected_state_probes=16 failures=0
popd
exit /b 0
:normal
cl %CFLAGS% /DREVIEW_HEADER_%~1 /c tests\phase2-review\probe_opaque.c /Fo"%OUT%\normal_%~1.obj" >"%OUT%\normal_%~1.log" 2>&1
if errorlevel 1 exit /b 1
echo OPAQUE_NORMAL header=%~1 compiled=1
exit /b 0
:reject
cl %CFLAGS% /DREVIEW_HEADER_%~1 /DREVIEW_PROBE_%~2 /c tests\phase2-review\probe_opaque.c /Fo"%OUT%\negative_%~1_%~2.obj" >"%OUT%\negative_%~1_%~2.log" 2>&1
if not errorlevel 1 exit /b 1
findstr /C:"error C2027" /C:"error C2065" /C:"error C2061" /C:"error C2070" "%OUT%\negative_%~1_%~2.log" >nul
if errorlevel 1 exit /b 1
echo OPAQUE_REJECT header=%~1 probe=%~2 incomplete_or_undeclared=1
exit /b 0
:failed
echo OPAQUE_RESULT failed=1
popd
exit /b 1
