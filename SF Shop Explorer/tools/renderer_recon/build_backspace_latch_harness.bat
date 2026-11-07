@echo off
rem Compiles the Backspace latch harness against the vendored ImGui sources.
rem Same shape as build_cache_harness.bat: vcvarsall first so the CRT headers
rem resolve, then cl.exe by full path.
setlocal
set SRC_DIR=%~dp0
set IMGUI_DIR=%~dp0..\..\Depends\imgui
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
"%CL_EXE%" /nologo /std:c++17 /EHsc /W3 /O1 /DIMGUI_DISABLE_OBSOLETE_FUNCTIONS ^
  /I"%IMGUI_DIR%" ^
  /Fe:"%SRC_DIR%backspace_latch_harness.exe" ^
  "%SRC_DIR%backspace_latch_harness.cpp" ^
  "%IMGUI_DIR%\imgui.cpp" "%IMGUI_DIR%\imgui_draw.cpp" "%IMGUI_DIR%\imgui_tables.cpp" "%IMGUI_DIR%\imgui_widgets.cpp"
exit /b %ERRORLEVEL%
