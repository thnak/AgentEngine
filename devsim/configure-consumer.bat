@echo off
REM Authentic consumer test: what a developer does after reading the AgentEngine docs.
REM A parent project that add_subdirectory()s the vendored engine and links agentengine::core.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%~dp0.."
cmake -S devsim\consumer-cmake -B devsim\consumer-cmake\build -G Ninja -DCMAKE_BUILD_TYPE=Release
echo CONFIGURE_EXIT=%errorlevel%
