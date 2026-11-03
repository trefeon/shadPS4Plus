# PKG Extractor Quality Ports Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the three narrow extractor-safe quality fixes from `AzaharPlus/shadPS4Plus` into `trefeon/shadPS4Plus` without touching emulator code.

**Architecture:** Three isolated slices sharing no interfaces: padded Np decrypt in `pkg.cpp`, portable `fseeko`/`ftello` in `io_file.cpp`, POSIX quoting in two `.sh` wrappers. Each slice builds with the existing MSYS2 CLANG64 `pkg_extractor` target and re-runs `ctest cli_invalid_pkg`.

**Tech Stack:** C++23, Crypto++ AES-CBC (`aesCbcCfb128DecryptEntry` 16-byte blocks), zlib, CMake 3.20+ Ninja `clang++`, MSYS2 CLANG64 `bash build/wconf` + `extractor/build/cryptobuild` + `wpack`, CTest `tests/cli_invalid_pkg.cmake`, POSIX `sh`.

**Spec:** N/A — spike findings in session 2026-11-03. Sources: parent issues `AzaharPlus/shadPS4Plus#36` (Np OOB), `#38` (`fseeko64` macOS), `#30`/`#28` (spaces/special chars), `#23`/`#27` already in HEAD `9d2e7612`. Emulator perf/iGPU PRs (`5572`, `5571`, `5100`, `3404`, `3396`, `5459`) explicitly out of scope.

## Global Constraints

- Windows-only standalone extractor; build entry is `extractor/CMakeLists.txt` via `bash build/wconf` in MSYS2 CLANG64; never configure repo root.
- Preserve CLI contract: exit `0` success, `1` invalid, `101/102/103` base/patch/DLC via `--check-type`; stdout strings `doesn't appear to be a valid PKG file`, `PKG header is truncated` pinned by `ctest cli_invalid_pkg`.
- Validate every PKG/SFO/PFS header, range (`IsRangeWithin`), AES-block alignment, crypto, and contain all writes beneath root (`NormalizeContainedPath`); quote all batch paths.
- Do NOT port emulator VFS (`#5568` class) or PKG-removal (`#2702` class); do NOT touch `src/video_core`, `src/shader_recompiler`, emulator GUI, Vulkan, FSR.
- No placeholders, no new scope (no directory-sweep batch, no progress-bar redesign, no AppImage CWD change).

---

## File map

- Modify: `src/core/file_format/pkg.cpp:448-456` (alignment gate), `:892-911` (Np read/decrypt/write).
- Modify: `src/common/io_file.cpp:24-26` (`_MSC_VER` define block), `:380-412` (`Seek` + `Tell`).
- Modify: `extractor/install_pkg.sh` (two `` `dirname $0` `` call sites + backticks).
- Modify: `extractor/batch_install.sh` (unquoted `$f` loop + backtick call).
- Test: existing `ctest -R cli_invalid_pkg` (all tasks); plus throwaway synthetic checks per task, deleted before commit.
- Docs: `documents/building-windows.md` unchanged; no doc edit needed.

### Task 1: Np entry padded decrypt (parent #36)

**Files:**
- Modify: `src/core/file_format/pkg.cpp:448-456`
- Modify: `src/core/file_format/pkg.cpp:892-911`
- Test: `ctest -R cli_invalid_pkg --output-on-failure` + throwaway padded-block check (deleted after)

**Interfaces:**
- Consumes: `PKGEntry { u32_be id, filename_offset, flags1, flags2, u32_be offset, u32_be size; u64_be padding; }` (`sizeof == 32`), `Crypto::aesCbcCfb128DecryptEntry(ivkey32, cipher, plain)` which steps `i += AES::BLOCKSIZE (16)` and reads full blocks.
- Produces: `decNp` sized to logical `entry.size`; on-disk read sized to `paddedSize`; no signature change for later tasks.

- [ ] **Step 1: Reproduce the fail-closed case on paper**

Run: read `src/core/file_format/pkg.cpp:448-456`
Expected: gate `entry.size % 16 != 0 → failreason "Encrypted PKG entry size is not AES block aligned", return false` fires for a 532-byte `npbind.dat` (532 % 16 == 4), so a valid PKG hard-fails instead of decrypting.

- [ ] **Step 2: Relax the gate to range-check the padded span**

```cpp
if ((entry.id == 0x400 || entry.id == 0x401 || entry.id == 0x402 || entry.id == 0x403) &&
    entry.size % CryptoPP::AES::BLOCKSIZE != 0) {
    const u64 padded_size =
        (entry.size + CryptoPP::AES::BLOCKSIZE - 1) / CryptoPP::AES::BLOCKSIZE *
        static_cast<u64>(CryptoPP::AES::BLOCKSIZE);
    if (!IsRangeWithin(entry.offset, padded_size, package_size)) {
        failreason = "Encrypted PKG entry padded range is outside the file";
        return false;
    }
}
```

Run: `bash build/wconf` from MSYS2 CLANG64
Expected: `extractor/build/pkg_extractor.exe` rebuilds, gate compiles.

- [ ] **Step 3: Read padded bytes, decrypt full blocks, trim to logical size**

Replace `src/core/file_format/pkg.cpp:892-911` body with:

```cpp
const u64 entry_size = entry.size;
const u64 padded_size = encrypted_np_entry
                            ? (entry_size + CryptoPP::AES::BLOCKSIZE - 1) /
                                  CryptoPP::AES::BLOCKSIZE *
                                  static_cast<u64>(CryptoPP::AES::BLOCKSIZE)
                            : entry_size;
std::vector<u8> data(static_cast<size_t>(padded_size));
if (!file.Seek(static_cast<s64>(static_cast<u32>(entry.offset))) ||
    file.Read(data) != data.size()) {
    failreason = "Failed to read PKG entry data";
    return false;
}
```

and after the existing `ivKey` derive:

```cpp
decNp.resize(data.size());
PKG::crypto.aesCbcCfb128DecryptEntry(ivKey, data, decNp);
output_data = std::span<const u8>(decNp.data(), static_cast<size_t>(entry_size));
```

Keep the existing `out.WriteRaw(output_data.data(), output_data.size())` call unchanged so only `entry_size` bytes hit disk.

- [ ] **Step 4: Verify no behavior change for aligned entries and gate still holds**

Run: `ctest --test-dir extractor/build -R cli_invalid_pkg --output-on-failure`
Expected: PASS (missing/empty/truncated fixtures exit `1` with pinned strings).

Run: synthetic check with a hex fixture where `entry.size = 532`, `entry.offset` near EOF with only 532 bytes left: `Open` must now fail with `padded range is outside the file`, not read past EOF.
Expected: fail-closed with the new message; aligned entries byte-identical.

- [ ] **Step 5: Commit**

```bash
git add src/core/file_format/pkg.cpp
git commit -m "fix pkg Np entries with non-block size decrypt padded block then trim"
```

### Task 2: Portable seek/tell (parent #38)

**Files:**
- Modify: `src/common/io_file.cpp:24-26`
- Modify: `src/common/io_file.cpp:380-412`
- Test: `ctest -R cli_invalid_pkg --output-on-failure` + `bash build/wconf` rebuild

**Interfaces:**
- Consumes: `IOFile::Seek(s64, SeekOrigin)`, `IOFile::Tell()`, `errno`/`LOG_ERROR` pattern already in file.
- Produces: same two signatures; later tasks unaffected.

- [ ] **Step 1: Confirm the break**

Run: read `src/common/io_file.cpp:24-26` and `:380-412`
Expected: only `_MSC_VER` maps `fseeko→_fseeki64`; `Seek` calls `fseeko64` unconditionally, `Tell` calls `ftello`. macOS has no `fseeko64` (`off_t` already 64-bit) so the extractor target fails to compile there exactly as `#38` reports. Windows/Linux unaffected.

- [ ] **Step 2: Add the portability shim**

```cpp
#ifdef _MSC_VER
#define fileno _fileno
#define fseeko _fseeki64
#define ftello _ftelli64
#elif defined(__APPLE__)
#define fseeko64 fseeko
#define ftello64 ftello
#endif
```

- [ ] **Step 3: Route call sites through the shim**

```cpp
const auto seek_result = fseeko(file, offset, ToSeekOrigin(origin)) == 0;
```

and in `Tell`:

```cpp
return ftello(file);
```

Leave the `errno = 0`, `LOG_ERROR`, and return-value logic byte-identical.

- [ ] **Step 4: Rebuild and re-run gate**

Run: `bash build/wconf`
Expected: `pkg_extractor.exe` links, no new warnings in `io_file.cpp`.

Run: `ctest --test-dir extractor/build -R cli_invalid_pkg --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/common/io_file.cpp
git commit -m "fix portable large-file seek for macOS builds"
```

### Task 3: Quote POSIX wrappers (parent #30/#28, `.sh` only)

**Files:**
- Modify: `extractor/install_pkg.sh`
- Modify: `extractor/batch_install.sh`
- Test: `sh -n` syntax + spaced-path dry run (throwaway dirs, deleted after)

**Interfaces:**
- Consumes: `pkg_extractor.AppImage "$1" --check-type → 101/102/103`, `install_pkg.sh <pkg> --batch`, `batch_install.sh <dir>`.
- Produces: same CLI and exit codes; quoted paths only.

- [ ] **Step 1: Confirm the split**

Run: read `extractor/install_pkg.sh`, `extractor/batch_install.sh`, `extractor/install_pkg.bat`
Expected: `.bat` already quotes `"%~1"`, `"%gamesDir%"`, `"%installDir%"` — no change. `.sh` has two unquoted `` `dirname $0` `` backtick sites and `batch_install.sh` loops `$f` unquoted, so a spaced parent dir breaks the wrapper while `main.cpp` containment (`IsSafeWindowsPathComponent`, `NormalizeContainedPath`) stays correct. Do NOT touch `main.cpp`.

- [ ] **Step 2: Fix `install_pkg.sh` quoting only**

```sh
SCRIPT_DIR="$(dirname -- "$0")"
"${SCRIPT_DIR}/pkg_extractor.AppImage" "$1" --check-type

ret="$?"

if [ "$ret" -eq 0 ]; then
	echo "An error has occurred."
else
	if [ "$ret" -eq 101 ]; then
		echo The file is a base game, installing to $gamesDir
	elif [ "$ret" -eq 102 ]; then
		echo The file is a game update, installing to $gamesDir
	elif [ "$ret" -eq 103 ]; then
		echo The file is a dlc, installing to $addonsDir
		gamesDir=$addonsDir
	fi

	"${SCRIPT_DIR}/pkg_extractor.AppImage" "$1" "$gamesDir"
fi
```

Keep the `gamesDir`/`addonsDir` placeholder guards and `--batch` read guards unchanged.

- [ ] **Step 3: Fix `batch_install.sh` loop quoting only**

```sh
#!/bin/bash

oldIFS="$IFS"
IFS=$'\n'

for f in "$1"/*.pkg ; do
	"$(dirname -- "$0")/install_pkg.sh" "$f" --batch
done

IFS="$oldIFS"

echo "Press [enter] to close"
read
```

- [ ] **Step 4: Verify syntax and spaced paths**

Run: `sh -n extractor/install_pkg.sh && sh -n extractor/batch_install.sh`
Expected: no output (syntax OK).

Run: create throwaway `/tmp/spaced dir/` with a dummy `.pkg`, run `install_pkg.sh` with edited `gamesDir`/`addonsDir` pointing at throwaway dirs in `--batch` mode, confirm the extractor path resolves with spaces.
Expected: wrapper reaches `pkg_extractor` invocation (PKG itself then fails validation as dummy, exit `1`); no `dirname`/word-split error. Delete throwaway dirs.

- [ ] **Step 5: Commit**

```bash
git add extractor/install_pkg.sh extractor/batch_install.sh
git commit -m "fix sh wrappers for spaced paths"
```

---

## Explicitly out of scope (do not implement)

- Emulator renderer/shader work: `5572`, `5571`, `5100`, `3404`, `3396`, `5459`, `5468`, `5366`, FSR `host_passes/fsr_pass`, `5519`, driver issues `1739`/`3239`, pipeline cache `4805` — all touch `video_core`/`shader_recompiler`/drivers with no extractor consumer.
- Linux directory-sweep batch (`#24` asks for a folder loop; HEAD only has single-file `--batch`) — new scope, no upstream patch to pull.
- AppImage default-output CWD (`#29`) — wrapper packaging behavior, not `main.cpp` logic.
- Progress-bar redesign (`#25` already satisfied by `4faf965` per-file `Extracting file i of nfiles`).
- Plain-binary-vs-AppImage (`#31` already satisfied by `extractor/CMakeLists.txt` plain `pkg_extractor`).
- Exit-code semantics (`101/102/103`) — no issue requests a change; keep pinned.
