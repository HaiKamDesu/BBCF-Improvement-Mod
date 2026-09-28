@echo off
setlocal
set "RESEARCH=%~dp0"
set "PROJECT_DIR=C:\Users\Usuario\source\repos\HaiKamDesu\BBCF-Improvement-Mod\docs\Research\BBCF-Ghidra-Project"
set "PROJECT_NAME=BBCF"
set "GHIDRA_HEADLESS=D:\Programs\ghidra_11.4.2_PUBLIC\support\analyzeHeadless.bat"
set "SCRIPT_DIR=%RESEARCH%ghidra_scripts"
set "REPORT=%RESEARCH%TrainingResetPositionGhidraReport.txt"
"%GHIDRA_HEADLESS%" "%PROJECT_DIR%" "%PROJECT_NAME%" -process BBCF.exe -noanalysis -readOnly -scriptPath "%SCRIPT_DIR%" -postScript DecompileTrainingResetPosition.py "%REPORT%"
