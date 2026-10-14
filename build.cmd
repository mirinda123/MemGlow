@echo off
setlocal
set "CMAKE_EXE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%CMAKE_EXE%" -S "%~dp0." -B "%~dp0build" -G "Visual Studio 16 2019" -A x64
if errorlevel 1 exit /b 1
"%CMAKE_EXE%" --build "%~dp0build" --config Release --parallel
exit /b %errorlevel%
