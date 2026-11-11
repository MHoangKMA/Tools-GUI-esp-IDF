@echo off
rem Double-click to open the Xiaozhi Builder.
rem It first checks the environment (Python, tkinter, Git, ESP-IDF, toolchain, ESP-IDF Python packages) and
rem installs whatever is missing through XiaozhiSetup.exe, then opens the window.
setlocal
cd /d "%~dp0"
set "SETUP_LOG=%USERPROFILE%\.xiaozhi_builder_setup.log"

rem ---------------------------------------------------------------- Python 3
call :find_python
if not defined PY (
    echo Python 3 is missing: installing it with winget...
    where winget >nul 2>&1 && winget install --id Python.Python.3.12 -e --silent --accept-package-agreements --accept-source-agreements
    call :find_python
)
if not defined PY (
    echo.
    echo Python 3 was not found and could not be installed automatically.
    echo Install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH" and "tcl/tk"^), then run this file again.
    pause
    exit /b 1
)

%PY% -c "import tkinter" >nul 2>&1
if errorlevel 1 (
    echo This Python has no tkinter. Re-run the python.org installer, choose Modify, and tick "tcl/tk and IDLE".
    pause
    exit /b 1
)

rem ---------------------------------------------------------------- build environment
%PY% scripts\setup_env.py --check >"%SETUP_LOG%" 2>&1
if errorlevel 1 (
    echo Preparing the build environment ^(first run only, needs Internet^)...
    if exist "XiaozhiSetup.exe" (
        start /wait "" "XiaozhiSetup.exe"
    ) else (
        %PY% scripts\setup_env.py
    )
    %PY% scripts\setup_env.py --check >"%SETUP_LOG%" 2>&1
    if errorlevel 1 (
        echo.
        echo The environment is still incomplete:
        type "%SETUP_LOG%"
        echo.
        echo Check the Internet connection and run this file again.
        pause
        exit /b 1
    )
)

rem ---------------------------------------------------------------- build/ copied from another PC or folder
%PY% scripts\clean_stale.py

rem ---------------------------------------------------------------- open the Builder
set "PYW="
where pyw >nul 2>&1 && set "PYW=pyw -3"
if not defined PYW (where pythonw >nul 2>&1 && set "PYW=pythonw")
if defined PYW (
    start "" %PYW% scripts\builder_gui.py
) else (
    %PY% scripts\builder_gui.py
    if errorlevel 1 pause
)
endlocal
exit /b 0

:find_python
set "PY="
where py >nul 2>&1 && (py -3 -c "import sys" >nul 2>&1 && set "PY=py -3")
if defined PY exit /b 0
where python >nul 2>&1 && (python -c "import sys" >nul 2>&1 && set "PY=python")
if defined PY exit /b 0
for %%D in ("%LocalAppData%\Programs\Python\Python312" "%LocalAppData%\Programs\Python\Python313" "%LocalAppData%\Python\pythoncore-3.14-64") do (
    if exist "%%~D\python.exe" if not defined PY set PY="%%~D\python.exe"
)
exit /b 0
