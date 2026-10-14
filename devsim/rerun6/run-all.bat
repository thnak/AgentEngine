@echo off
REM Runs every variant, prints its exit code. Not part of the deliverable.
setlocal enabledelayedexpansion
cd /d "%~dp0"
for %%V in (concurrent trap routed bounded switch-default throw bad-route missing-label unbounded) do (
  echo.
  echo ########## %%~V ##########
  build\docpipeline.exe --variant=%%~V
  echo [exit code: !ERRORLEVEL!]
)
echo.
echo ########## routed --route=brief ##########
build\docpipeline.exe --variant=routed --route=brief
echo [exit code: !ERRORLEVEL!]
