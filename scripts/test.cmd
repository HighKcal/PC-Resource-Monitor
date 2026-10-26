@echo off
setlocal
pushd "%~dp0.."
set "COLLECTOR_OUT=%~1"
if not defined COLLECTOR_OUT set "COLLECTOR_OUT=build"
set "MONITOR_OUT=%~2"
if not defined MONITOR_OUT set "MONITOR_OUT=build-monitor"
"%COLLECTOR_OUT%\test_core.exe"
if errorlevel 1 goto failed
"%COLLECTOR_OUT%\test_windows.exe"
if errorlevel 1 goto failed
"%MONITOR_OUT%\test_monitor.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
