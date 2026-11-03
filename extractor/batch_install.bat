@echo off
setlocal

set "dir=%~1"
set "failed=0"

if not defined dir (
	echo Usage: %~nx0 "<directory>"
	pause
	exit /b 1
)

if not exist "%dir%\." (
	echo Directory not found: "%dir%"
	pause
	exit /b 1
)

for %%i in ("%dir%\*.pkg") do if exist "%%~fi" (
	call "%~dp0install_pkg.bat" "%%~fi" --batch
	if errorlevel 1 set "failed=1"
)

if "%failed%"=="0" (
	echo Batch install done
) else (
	echo Batch install completed with errors
)

pause
exit /b %failed%
