@echo off
call "%~dp0scripts\build-collector.cmd" %*
exit /b %errorlevel%
