@echo off
call "%~dp0scripts\build-benchmarks.cmd" %*
exit /b %errorlevel%
