<!--
SPDX-FileCopyrightText: 2025 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Build the PKG extractor for Windows

These instructions build the standalone `pkg_extractor` with MSYS2 CLANG64. They do not build the shadPS4 emulator.

## Install MSYS2 and dependencies

1. Install [MSYS2](https://www.msys2.org/).
2. Open the **MSYS2 CLANG64** terminal and update its packages:

   ```bash
   pacman -Syu
   ```

   If MSYS2 asks you to close the terminal, reopen **MSYS2 CLANG64** and run the update command again.
3. Install the build dependencies:

   ```bash
   pacman -S --needed git make mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja mingw-w64-clang-x86_64-zlib mingw-w64-clang-x86_64-llvm-tools zip
   ```

## Clone and build

Clone the repository with its Crypto++ submodule, then run the repository's Windows build helper from the MSYS2 CLANG64 terminal:

```bash
git clone --recurse-submodules <repository-url>
cd <repository-directory>
bash build/wconf
```

The helper can also be run from a different working directory by passing its path to Bash. It builds the Crypto++ static library, configures CMake against `extractor/`, and builds `pkg_extractor` in Release mode.

The executable is written to:

```text
extractor/build/pkg_extractor.exe
```

## Package for Explorer use

To create a portable folder and ZIP containing the executable, its CLANG64 runtime DLLs, and the Windows installer scripts, pass a version label:

```bash
bash extractor/build/wpack personal-2026.10.30
```

The version label may contain letters, digits, periods, underscores, and hyphens. Without an argument, the helper uses the short Git revision and adds a local date suffix when the working tree is modified. The folder and ZIP are written under `extractor/build/`. Edit `install_pkg.bat` in the folder to set the games and addons directories before using the batch installer.
