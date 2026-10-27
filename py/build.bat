@echo off
chcp 65001 >nul
echo 正在打包 NCM 转 MP3...

python -m PyInstaller ^
  --noconfirm ^
  --onefile ^
  --windowed ^
  --name "NCM转MP3" ^
  --icon "icon.ico" ^
  --collect-all tkinterdnd2 ^
  ncm_gui.py

if errorlevel 1 (
    echo.
    echo 打包失败，请确认已安装：
    echo     pip install pyinstaller cryptography tkinterdnd2
    pause
    exit /b 1
)

echo.
echo 完成！产物在 dist\NCM转MP3.exe
pause
