Unreleased - pkg_extractor PFS extraction fixes
=================

- Read the full PFS image declared by the header instead of the
  `pfs_cache_size * 2` window, so small (DLC/unlock) and large (game)
  packages both validate and decrypt.
- PFSC view reuses the decrypted buffer instead of copying it (avoids a
  third full-image allocation on large games).
- Per-file sector reads use the full image size for range checks and clamp
  the final 0x11000-byte window to the PKG with zero-padding.
- Inode table parsing restarts the record stride at each PFSC block
  (records never span blocks); previously block 2+ was read out of phase
  and valid inodes looked like zeros.
- Verified against Crash Team Racing Nitro Fueled (CUSA13795): base game
  (12.6GB, 582 files), update v1.21 (7.9GB, 176 entries), 3 DLC unlock
  packages. See documents/pkg-extraction.md for details.

v0.4.0 31/10/2024 - codename divicius
=================

- Shader recompiler fixes
- Emulated support for cpus that doesn't have SSE4.2a (intel cpus)
- Frame graph + Precise 60 fps timing
- Save data: fix nullptr & concurrent file write
- Auto Update
- Error dialog implementation
- Swapchain recreation and window resizing
- Add playback of background/title music in game list
- Kernel: Quiet sceKernelWaitEventFlag error log on timeout
- Improve keyboard navigation in game list
- core/memory: Pooled memory implementation
- Fix PKG loading
- replace trophy xml assert with error
- Refactor audio handling with range checks, buffer threshold, and lock
- audio_core: Fix return value types and shift some error handling to library
- Devtools: PM4 Explorer
- Initial support of Geometry shaders
- Working touchpad support
- net: Stub sceNetErrnoLoc
- Add support to click touchpad using back button on non PS4/5 controllers
- Multiple Install Folders
- Using a more standard data directory for linux
- video_core: Implement sceGnmInsertPushColorMarker
- ime_dialog: Initial implementation
- Network libs fixes
- Use GetSystemTimePreciseAsFileTime to fix fps timing issues
- Added adaptive mutex initializer
- Small Np + trophy fixes
- Separate Updates from Game Folder
- Minor Fixes for Separate Update Folder
- AvPlayer: Do not align w/h to 16 with vdec2
- Improve sceSystemServiceReceiveEvent stub
- renderer_vulkan: Commize and adjust buffer bindings
- Add poll interval to libScePad
- Add more surface format mappings.
- vulkan: Report only missing format feature flags.
- IME implementation
- Videodec2 implementation
- path_util: Make sure macOS has current directory set and clean up path code.
- Load LLE modules from sys_modules/GAMEID folder

v0.3.0 23/09/2024 - codename broamic
=================

- Cheat/Patching support
- DLC support
- New translations support (26 languages)
- Support for unlocking trophies
- Support for more controllers (Dualshock and Xbox)
- Many GUI improvements
- AVplayer

v0.2.0 15/08/2024 - codename validptr
=================
- Adding macOS support
- Big shader recompiler improvements
- Core improvements
- GUI improvements

v0.1.0 01/07/2024 - codename madturtle
=================
- Added a shader recompiler, with this we have a lot of games that starts to work
- Rewrote a big part of core

v0.0.3 23/03/2024 - codename salad
=================
- Switching to std::thread
- Use unique_ptr where possible
- Replace printf/scanf with type safe fmt
- Implemented sceKernelGetProcessTime
- Implemented sceKernelGetProcessTimeCounter, sceKernelGetProcessTimeCounterFrequency
- Pause emu with P button
- Timers rewrote with std::chrono
- Added sceSystemServiceGetStatus
- Initial FileSystem implementation
- Initial TLS work
- New logging implementation
- Some functions implemented for userService, systemService
- Added sceAudioOut module and output using SDL audio

v0.0.2 21/10/2023
=================
- Using cstdint header in variable types
- run_main_entry: Rewrite in asm for stack setup
- Printf libc implementation for work with sysv_abi
- Initial pad emulation (only digital pad atm)
- Implemented sceVideoOutIsFlipPending
- Added auto stubs, now unsupported hle function will resolve as empty stubs
- Rewrote libc_cxa functions
- Libc implementations ( _ZdlPv,_Znwm,rand,_Fsin,qsort,free,strncpy,memmove,atan2f,pow,_Sin)
- ET_SCE_DYNAMIC behaves as valid for execution now
- Initial FileSystem work (not yet usable)

v0.0.1 29/09/2023
=================
First public release. Everything is new.