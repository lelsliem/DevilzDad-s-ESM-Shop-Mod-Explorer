@echo off
rem Compiles the BC4-mean harness (metalness/roughness decode). Kept as a .bat
rem because the compiler path contains spaces; vcvarsall is needed so the CRT
rem headers and libs resolve.
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
  /Fe:"%SRC_DIR%bc4_mean_harness.exe" ^
  "%SRC_DIR%bc4_mean_harness.cpp"
exit /b %ERRORLEVEL%
