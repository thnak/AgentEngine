@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%~dp0.."
cmake -S devsim\consumer-cmake-setform -B devsim\consumer-cmake-setform\build -G Ninja -DCMAKE_BUILD_TYPE=Release
echo CONFIGURE_EXIT=%errorlevel%
if errorlevel 1 goto :eof
REM Prove the tests/examples really are gone from the generated build.
findstr /c:"agentengine_example_01_hello_agent" devsim\consumer-cmake-setform\build\build.ninja >nul 2>&1
if errorlevel 1 (echo EXAMPLES_ABSENT=yes) else (echo EXAMPLES_ABSENT=no)
findstr /c:"compile_fail" devsim\consumer-cmake-setform\build\build.ninja >nul 2>&1
if errorlevel 1 (echo TESTS_ABSENT=yes) else (echo TESTS_ABSENT=no)
cmake --build devsim\consumer-cmake-setform\build --target myapp -j 4
echo BUILD_EXIT=%errorlevel%
