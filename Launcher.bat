@echo off
rem Double-click start for the installer / launcher window (recomp\launcher.py).
rem The release bundle brings its own Python in python\ (and the compiler in toolchain\): nothing to install.
rem From a plain source checkout it needs Python 3 from python.org (with "tcl/tk", on by default).
cd /d "%~dp0"
if exist "python\pythonw.exe" (start "" "python\pythonw.exe" -I recomp\launcher.py %* & exit /b)
where pyw >nul 2>nul && (start "" pyw -3 -I recomp\launcher.py %* & exit /b)
where pythonw >nul 2>nul && (start "" pythonw -I recomp\launcher.py %* & exit /b)
where python >nul 2>nul && (start "" python -I recomp\launcher.py %* & exit /b)
echo Python 3 was not found on this PC.
echo.
echo Install it from https://www.python.org/downloads/ (keep "Add python.exe to PATH" ticked),
echo then start this file again.
echo.
pause
