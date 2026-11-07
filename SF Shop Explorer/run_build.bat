@echo off
setlocal
call "D:\Visual Studios 2022\Install\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 (
  echo VCVARS FAILED
  exit /b 1
)
echo VCVARS OK
cd /d "D:\Projects\Address Library\SF-Shop\SF Shop Explorer"
"C:\Program Files\xmake\xmake.exe" -y
echo XMAKE EXIT: %errorlevel%
