@echo off
setlocal
pushd "%~dp0\..\.."
set "OUT=%~1"
if not defined OUT set "OUT=build-phase1-20261104\retention-only\module-build"
set "SOURCE=%~2"
if not defined SOURCE set "SOURCE=."
call scripts\setup-msvc.cmd
if errorlevel 1 goto failed
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto failed
rem Only SQLite and retention: no monitor.lib or explicit GUI import libraries.
cl /nologo /std:c17 /O2 /MT /w /DSQLITE_THREADSAFE=1 /DSQLITE_DQS=0 /DSQLITE_DEFAULT_MEMSTATUS=0 /DSQLITE_ENABLE_DBSTAT_VTAB /c "%SOURCE%\third_party\sqlite\sqlite3.c" /Fo"%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
set "CFLAGS=/nologo /std:c17 /W4 /WX /O2 /MT /utf-8 /I"%SOURCE%\prototype" /I"%SOURCE%\third_party\sqlite""
cl %CFLAGS% /c "%SOURCE%\prototype\experiment_retention.c" /Fo"%OUT%\experiment_retention.obj"
if errorlevel 1 goto failed
cl %CFLAGS% tests\phase1-retention\test_retention_module.c /Fo"%OUT%\test_retention_module.obj" /Fe"%OUT%\test_retention_module.exe" "%OUT%\experiment_retention.obj" "%OUT%\sqlite3.obj"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
