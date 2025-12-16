@echo off
setlocal

REM Runner for generate_localization_bindings.py that tolerates varying Python installs.
set "SCRIPT=%~dp0generate_localization_bindings.py"

set "PYTHON_CMD="
py -3 --version >nul 2>&1 && set "PYTHON_CMD=py -3"
if not defined PYTHON_CMD py --version >nul 2>&1 && set "PYTHON_CMD=py"
if not defined PYTHON_CMD python --version >nul 2>&1 && set "PYTHON_CMD=python"
if not defined PYTHON_CMD python3 --version >nul 2>&1 && set "PYTHON_CMD=python3"

if not defined PYTHON_CMD (
  echo [Localization] Python 3 is required to regenerate localization bindings.
  echo [Localization] Install Python and ensure either "py" or "python" is on PATH.
  exit /b 1
)

%PYTHON_CMD% "%SCRIPT%" --input "%~1" --output "%~2"
exit /b %ERRORLEVEL%
