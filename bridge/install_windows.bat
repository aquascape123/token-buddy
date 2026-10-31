@echo off
rem Token Buddy - installa il bridge sul PC (serve Python 3 installato)
where python >nul 2>nul
if errorlevel 1 (
  echo Python non trovato. Installalo da https://www.python.org/downloads/
  echo e spunta "Add python.exe to PATH" durante l'installazione.
  pause
  exit /b 1
)
python "%~dp0install_windows.py" %*
pause
