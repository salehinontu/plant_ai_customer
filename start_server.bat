@echo off
rem Starts the PlantRail server (website + API).
rem   start_server.bat           normal mode, talks to your ESP32 boards (see .env)
rem   start_server.bat --mock    demo mode with simulated hardware, no ESP32 needed
cd /d "%~dp0"

if not exist .venv (
  echo Creating Python virtual environment...
  python -m venv .venv || goto :err
)
.venv\Scripts\python.exe -m pip install -q -r server\requirements.txt || goto :err

if not exist .env (
  copy .env.example .env >nul
  echo Created .env from .env.example - open it and fill in your values.
)

echo.
echo  PlantRail is starting on http://localhost:8000
echo.
.venv\Scripts\python.exe -m server %*
goto :eof

:err
echo.
echo Setup failed. Is Python 3.10 or newer installed and on PATH?
pause
