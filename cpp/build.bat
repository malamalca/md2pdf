@echo off
rem Build md2pdf.exe with VS 2022 Build Tools + bundled CMake/Ninja
setlocal

set "VCVARS=D:\Dev\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "CMAKE=D:\Dev\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=D:\Dev\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

call "%VCVARS%" || exit /b 1
"%CMAKE%" -G Ninja -S "%~dp0." -B "%~dp0build" || exit /b 1
"%NINJA%" -C "%~dp0build" || exit /b 1

echo.
echo Build OK: %~dp0build\md2pdf.exe
endlocal
