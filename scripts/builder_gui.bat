@echo off
rem Double-click to open the Xiaozhi Builder (no console window when pythonw is available)
cd /d "%~dp0.."
where pythonw >nul 2>&1
if %errorlevel%==0 (
    start "" pythonw scripts\builder_gui.py
) else (
    python scripts\builder_gui.py
)
