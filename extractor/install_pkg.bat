@echo off
setlocal

set "par=%~2"
set "gamesDir=PATH_TO_GAMES_DIR"
set "addonsDir=PATH_TO_ADDONS_DIR"

if "%gamesDir%"=="PATH_TO_GAMES_DIR" (
	echo You need to update gamesDir with the games path used by ShadPs4
	if not "%par%"=="--batch" pause
	exit /b 1
)

if "%addonsDir%"=="PATH_TO_ADDONS_DIR" (
	echo You need to update addonsDir with the addons path used by ShadPs4
	if not "%par%"=="--batch" pause
	exit /b 1
)

"%~dp0pkg_extractor.exe" "%~1" --check-type
set "pkgType=%errorlevel%"
set "installDir="

if "%pkgType%"=="101" (
	echo The file is a base game, installing to "%gamesDir%"
	set "installDir=%gamesDir%"
)
if "%pkgType%"=="102" (
	echo The file is a game update, installing to "%gamesDir%"
	set "installDir=%gamesDir%"
)
if "%pkgType%"=="103" (
	echo The file is a dlc, installing to "%addonsDir%"
	set "installDir=%addonsDir%"
)

if not defined installDir (
	echo An error has occurred while checking the package type
	if not "%par%"=="--batch" pause
	exit /b 1
)

"%~dp0pkg_extractor.exe" "%~1" "%installDir%"
set "installResult=%errorlevel%"
if not "%installResult%"=="0" (
	echo An error has occurred while installing the package
)

if not "%par%"=="--batch" pause
exit /b %installResult%
