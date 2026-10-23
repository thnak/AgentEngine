@echo off
REM devsim/rerun3/build.bat -- consumer build of the docs-only pipeline program.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0"

cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DAGENTENGINE_VENDOR_ASIO=OFF ^
  -DAGENTENGINE_ASIO_ROOT="C:/Users/thanh/Git/AgentEngine/build-dev/_deps/ae_vendored_asio-src" ^
  %*
if errorlevel 1 exit /b 1

cmake --build build --target docpipe -j 8
exit /b %errorlevel%
