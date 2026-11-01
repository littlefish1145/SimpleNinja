@echo off
rem Run a command inside the MSVC 14.44 (VS 2022) x64 developer environment.
rem Usage: scripts\msvcenv.bat <command> [args...]
rem
rem This also lifts the Low mandatory-integrity label that the source tree
rem carries: a Low-integrity process cannot write into Medium-integrity
rem directories (e.g. %TEMP%\...), which makes builds of test projects fail
rem with "Permission denied".  Anything we build here should be Medium.
call "D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set VSLANG=1033
set PATH=D:\NASM;%PATH%
for %%f in (build\sninja.exe build\sninja_test.exe build\sninja_core.lib) do (
  if exist "%%f" icacls "%%f" /setintegritylevel M >nul 2>&1
)
%*
