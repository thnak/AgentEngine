@echo off
REM Build devsim/rerun7 with the repo's own toolchain (VS 18 Community + its bundled Ninja),
REM exactly as devsim/*.bat already do. Not part of the two-file deliverable; a convenience.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%~dp0"

if not exist build (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  echo CONFIGURE_EXIT=%errorlevel%
)
cmake --build build --target docpipeline -j 4
echo BUILD_EXIT=%errorlevel%
