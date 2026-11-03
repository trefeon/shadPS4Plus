# PKG extraction — PFS fixes and verification

Standalone `pkg_extractor` (`extractor/`, built via `bash build/wconf` in
MSYS2 CLANG64). Validates PKG/SFO/PFS headers, ranges, crypto, and keeps
all writes contained beneath the output root. Exit codes: `0` success,
`1` invalid, `101` base game / `102` patch / `103` DLC via `--check-type`.

## PFS fixes (all in `src/core/file_format/pkg.cpp`)

| Symptom | Cause | Fix |
|---|---|---|
| DLC/unlock PKGs rejected with `Invalid PFS image range or size` | Read window was `pfs_cache_size * 2`, which overshoots the declared image on small packages | Read the full header `pfs_image_size`, still range-checked |
| Base/update games failed with `PFSC sector map is outside the data range` | Same window: only `cache*2` bytes decrypted while the sector map spans the whole image | Same fix; sector offsets now resolve within the full decrypted image |
| Update stopped mid-extract (`entry 7`, later `entry 12`) | Per-file sector bound and sector reads used the same short window | Bound checks use the header image size; reads stream per-sector from the file on demand |
| Last file of an image failed to read | Fixed `0x11000` reads overran EOF on the final sector | Clamp the read to the PKG and zero-pad the tail (only covering bytes are consumed) |
| Base game stopped at inode 390 with `Invalid PFS inode record` | Inode parser stitched records across PFSC block boundaries; PFS records never span blocks, so block 2+ was read out of phase and valid inodes looked like zeros | Restart the record stride at offset zero for each inode-table block |
| 13.5GB image needs ~27GB transient RAM | Three full-image copies (encrypted, decrypted, PFSC view) | PFSC view moves (then erases the prefix of) the decrypted buffer instead of copying it |

Related: Np entries whose size is not AES-block-aligned (e.g. 532-byte
`npbind.dat`) are decrypted as a padded final block and trimmed to the
logical size instead of failing validation.

## Verification (Crash Team Racing Nitro Fueled, CUSA13795)

| Package | `--check-type` | Extract |
|---|---|---|
| Base game (12.6GB) | 101 base | RC=0, 582 files / 14.2GB, `eboot.bin` 50.9MB with valid SELF header (`4F 15 3D 1D`) |
| Update v1.21 (7.9GB) | 102 patch | RC=0, 176 entries, 8.7GB incl. `eboot.bin`, both `sce_discmap` plts |
| Preorder / Deluxe / Firehawk DLC (1MB each) | 103 DLC | RC=0 each, correct `sce_sys` layouts (param.sfo, license.dat, icons) |

Every file is size-verified against its inode after zlib decompression;
any mismatch aborts with a non-zero exit. `ctest -R cli_invalid_pkg`
still passes (missing/empty/truncated fixtures).

## Notes

- Inode records are 104 bytes on a 168-byte stride and never cross a
  64KiB PFSC block; each table block restarts the stride at offset zero.
- PFS image hash was verified against the header digest before debugging,
  ruling out a corrupt download.
- Full-image reads peak near 27GB commit on a 13.5GB game; fine on 32GB
  RAM, paging past that. A streaming inode/file path would remove the
  peak but is not needed for correctness.
