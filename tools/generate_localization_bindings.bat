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

  if exist "%~2" (
    echo [Localization] Python is missing, but a previously generated header was found. Skipping regeneration.
    exit /b 0
  )

  echo [Localization] Cannot continue because no existing header is available at "%~2".
  exit /b 1
)

%PYTHON_CMD% "%SCRIPT%" --input "%~1" --output "%~2"
set "EXIT_CODE=%ERRORLEVEL%"

if %EXIT_CODE% neq 0 (
  echo [Localization] Failed to generate bindings with %PYTHON_CMD% (exit code %EXIT_CODE%).
  if exist "%~2" (
    echo [Localization] Using the existing header at "%~2". Fix the generator to refresh it.
    exit /b 0
  )
)

exit /b %EXIT_CODE%
