@echo off
call "%~dp0scripts\build-monitor.cmd" %*
exit /b %errorlevel%
