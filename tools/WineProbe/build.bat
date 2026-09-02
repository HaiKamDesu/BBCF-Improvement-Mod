@echo off
REM Builds WineProbe.exe. 32-bit on purpose: the mod is Win32, and MFT/HID
REM availability is per-bitness, so a 64-bit probe would answer the wrong question.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /EHsc /W3 /MT /Fe:WineProbe.exe WineProbe.cpp ole32.lib oleaut32.lib user32.lib advapi32.lib
echo BUILD_EXIT=%ERRORLEVEL%
