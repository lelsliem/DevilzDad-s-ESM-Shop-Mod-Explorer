@echo off
rem Compiles the CDB cache round-trip harness. Kept as a .bat because the
rem compiler path contains spaces and cmd.exe quoting is the least surprising
rem way to launch it from Git Bash. vcvarsall is needed so the CRT headers
rem and libs resolve; without it cl.exe has no INCLUDE/LIB at all.
setlocal
set SRC_DIR=%~dp0
set VCVARS=D:\Visual Studios 2022\Install\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo FAIL: vcvars64.bat not found at %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (
  echo FAIL: vcvars64.bat failed
  exit /b 1
)
set CL_EXE=D:\Visual Studios 2022\Install\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
if not exist "%CL_EXE%" (
  echo FAIL: compiler not found at %CL_EXE%
  exit /b 1
)
"%CL_EXE%" /nologo /std:c++17 /EHsc /W3 /O1 ^
  /Fe:"%SRC_DIR%cdb_cache_harness.exe" ^
  "%SRC_DIR%cdb_cache_harness.cpp"
exit /b %ERRORLEVEL%