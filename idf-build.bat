@echo off
set MSYSTEM=
set IDF_TOOLS_PATH=E:\ESP\Espressif
call E:\ESP\esp-latest\esp-idf\export.bat
if errorlevel 1 exit /b 1
cd /d %~dp0
idf.py %*
