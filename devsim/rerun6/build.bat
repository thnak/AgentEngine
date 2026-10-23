@echo off
REM Build helper for devsim/rerun6. Not part of the deliverable -- it only puts the MSVC toolchain
REM on PATH (a Windows environment concern, not an AgentEngine documentation concern) and then runs
REM the two documented commands from ApiFirstAppReference.tsx:252.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set NINJA="C:\PROGRA~1\MICROS~4\18\COMMUN~1\Common7\IDE\COMMON~1\MICROS~1\CMake\Ninja\ninja.exe"
cd /d "%~dp0"
if "%1"=="configure" goto configure
if "%1"=="build" goto build
if "%1"=="run" goto run
echo usage: build.bat configure ^| build ^| run
exit /b 2

:configure
cmake -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=%NINJA% -DCMAKE_BUILD_TYPE=Release
exit /b %ERRORLEVEL%

:build
cmake --build build --target docpipeline
exit /b %ERRORLEVEL%

:run
build\docpipeline.exe %2 %3
exit /b %ERRORLEVEL%
