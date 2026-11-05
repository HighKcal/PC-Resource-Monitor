@echo off
rem Call after build-sources.cmd and setting CFLAGS. Arguments:
rem list-variable output-directory source-root object-prefix extra-compiler-flags
rem BUILD_OBJECTS is returned with quoted paths, in source-list order.
rem No setlocal: the returned object list intentionally reaches the caller.
call set "BLD_SOURCES=%%%~1%%"
set "BUILD_OBJECTS="
for %%f in (%BLD_SOURCES%) do (
  cl %CFLAGS% %~5 /c "%~3\%%f.c" /Fo"%~2\%~4%%~nf.obj"
  if errorlevel 1 exit /b 1
  call set "BUILD_OBJECTS=%%BUILD_OBJECTS%% "%~2\%~4%%~nf.obj""
)
exit /b 0
