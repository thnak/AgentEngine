@echo off
REM The documented-path test: a consumer that pre-sets the engine's build options so it does not
REM inherit the engine's own tests, examples, or tooling.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%~dp0.."

cmake -S devsim\consumer-cmake -B devsim\consumer-cmake\build-min -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DAGENTENGINE_BUILD_TESTS=OFF ^
  -DAGENTENGINE_BUILD_EXAMPLES=OFF
echo CONFIGURE_EXIT=%errorlevel%
if errorlevel 1 goto :eof

cmake --build devsim\consumer-cmake\build-min --target myapp -j 4
echo BUILD_EXIT=%errorlevel%
