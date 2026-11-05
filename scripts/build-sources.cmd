@echo off
rem Shared translation-unit lists, without extensions. Intentionally no setlocal.
rem Argument 1 is the source root; legacy preserved roots need no retention TU.
set "MONITOR_STATE_SOURCES=engine\monitor storage\monitor_storage"
if exist "%~1\engine\monitor_window.c" set "MONITOR_STATE_SOURCES=%MONITOR_STATE_SOURCES% engine\monitor_window"
if exist "%~1\storage\monitor_source.c" set "MONITOR_STATE_SOURCES=%MONITOR_STATE_SOURCES% storage\monitor_source"
if exist "%~1\storage\monitor_history.c" set "MONITOR_STATE_SOURCES=%MONITOR_STATE_SOURCES% storage\monitor_history"
set "BASELINE_SOURCES=detection\baseline"
if exist "%~1\detection\baseline_history.c" set "BASELINE_SOURCES=%BASELINE_SOURCES% detection\baseline_history"
set "COLLECTOR_SOURCES=collector\resource_core collector\resource_collector collector\monitor_identity"
set "MONITOR_SOURCES=%MONITOR_STATE_SOURCES% %BASELINE_SOURCES% %COLLECTOR_SOURCES%"
set "GROUPING_SOURCES=grouping\process_grouping grouping\process_grouping_windows grouping\program_groups"
set "PROTOTYPE_SOURCES=prototype\experiment"
set "PROTOTYPE_SUPPORT_SOURCES="
set "PROTOTYPE_MODULE_SOURCES=experiment"
set "PROTOTYPE_FOLDER=%~1\prototype"
if not "%~2"=="" for %%f in ("%~2") do (
  set "PROTOTYPE_FOLDER=%%~dpf"
  set "PROTOTYPE_MODULE_SOURCES=%%~nf"
)
for %%m in (experiment_retention experiment_store experiment_capture experiment_reducer experiment_history experiment_replay) do if exist "%PROTOTYPE_FOLDER%\%%m.c" (
  call set "PROTOTYPE_SOURCES=%%PROTOTYPE_SOURCES%% prototype\%%m"
  call set "PROTOTYPE_SUPPORT_SOURCES=%%PROTOTYPE_SUPPORT_SOURCES%% prototype\%%m"
  call set "PROTOTYPE_MODULE_SOURCES=%%PROTOTYPE_MODULE_SOURCES%% %%m"
)
exit /b 0
