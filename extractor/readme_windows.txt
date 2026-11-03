1. pkg_extractor.exe
	
	Can be called in command line.
	The first argument is the path to the pkg file to extract.
	The second argument is where to extract it.
	If no second argument is provided, it will be extracted next to the pkg file.
	
	Alternatively, you can drag and drop the pkg file onto the exe and it will extract it at the same location.


2. install_pkg.bat

	Needs to be edited before use.
	Set the values for gamesDir and addonsDir to the ones used in ShadPS4 (you can check in settings > paths).
	
	Can then be called in command line.
	The first argument is the path to the pkg file to extract.
	
	Alternatively, you can drag and drop the pkg file onto the bat and it will extract it where it needs to be.


3. batch_install.bat

	Set gamesDir and addonsDir in install_pkg.bat before use.
	Pass the directory containing the .pkg files as the first argument. Quote the path if it contains spaces.
	The script reports invalid packages or installation errors and returns a nonzero exit code. An empty directory is a no-op.
