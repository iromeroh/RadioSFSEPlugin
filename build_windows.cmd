@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

cd /d "C:\Program Files (x86)\Steam\steamapps\common\Starfield\RadioSFSEPlugin"
if errorlevel 1 exit /b %errorlevel%

cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b %errorlevel%

cmake --build build-vs --config Release
exit /b %errorlevel%
