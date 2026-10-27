@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
echo [1/3] resource
rc /nologo /fo ncm2mp3_gui.res ncm2mp3_gui.rc
if errorlevel 1 goto fail
echo [2/3] gui
cl /O2 /utf-8 /nologo ncm2mp3_gui.c ncm2mp3_gui.res bcrypt.lib comctl32.lib shell32.lib ole32.lib user32.lib gdi32.lib comdlg32.lib
if errorlevel 1 goto fail
echo [3/3] console
cl /O2 /utf-8 /nologo ncm2mp3.c bcrypt.lib
if errorlevel 1 goto fail
echo ALL OK
exit /b 0
:fail
echo BUILD FAILED
exit /b 1