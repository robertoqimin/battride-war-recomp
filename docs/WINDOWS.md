# Windows port

The port is developed on the `windows` branch. Changes to the SDK go upstream
to ps3recomp, since the build pins an SDK revision; until then they are kept as
`patches/ps3recomp/0002-vulkan-backend-on-windows.patch`.

## Status (November 12, 2026)

**The opening mission plays through on Windows.** On Windows 11 with a Ryzen 5
4600H and a GTX 1650 (the Linux reference machine), built with MSYS2 UCRT64:
data installation, the opening movie with sound, menus, tutorial and STG02 were
completed by hand, with the result screens, a save and the return to the menus.
Audio, a controller (XInput), keyboard and mouse, windowed and borderless
fullscreen (Alt+Enter), internal resolution 1x/2x/4x and FSR 1 work.

The GTK 4 launcher imports the ISO and builds the game on Windows from a
self-contained package that carries its own MSYS2 subset (stage 5). Not done:
an installer and licences (stage 6), testing on other machines, and
a build of this branch on Linux (every change is behind `_WIN32`, `WIN32` or
`__MINGW32__`, and was checked by reading, not by building there).

Performance in STG02 fighting, 60 s windows of the scripted route of
`scripts/bench_combat.py`, alternating builds:

| Build | Average | p95 frame | Worst 5 seconds |
|---|---|---|---|
| Windows, PGO, built by the launcher from the `--bundle --pgo` package (three runs) | 56.3 fps | 22.5 ms | 46.4 fps |
| Windows, PGO (three runs) | 52.9 fps | 24.2 ms | 45.4 fps |
| Windows, no PGO (four runs) | 41.9 fps | 32.1 ms | 33.3 fps |
| Linux, PGO (`docs/PERFORMANCE.md`) | 51.3 fps | 22.9 ms | 47.0 fps |

A 13-minute session played by hand on the PGO build averaged 55.2 fps; the
most crowded fights (650-880 draws) ran at 38-44 fps in 5-second windows.

## Building and running

Install [MSYS2](https://www.msys2.org) and, in the **MSYS2 UCRT64** shell:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,clang,lld,cmake,ninja,SDL2,vulkan-headers,glslang,spirv-tools,python,pkgconf}
```

Then follow `docs/BUILD.md` with these differences:

- Clone and patch with `core.autocrlf=false` (Git for Windows defaults to
  `true`, and the SDK patches then do not apply): `git -c core.autocrlf=false
  clone ...`, and set it in the port's checkout too.
- Decrypt the EBOOT with the official RPCS3 Windows build of the commit in
  `packaging/rpcs3.json`, `rpcs3-v0.0.43-20370-0b0fa082_win64_msvc.7z`
  (SHA-256 `2fc3324b15fb979b6ec6b578b3c695b03e369888dffea34d91b5ae3e4f08cead`):
  `rpcs3.exe --allow-any-location --headless --decrypt game/PS3_GAME/USRDIR/EBOOT.BIN`,
  and `--installfw` with the disc's `PS3UPDAT.PUP` for the codecs. Extract the
  ISO with 7-Zip; its "Headers Error" on these UDF images is expected.
- Build with the Windows preset; each lifted chunk takes up to 1.5 GB to
  compile, hence 3 jobs on 16 GB:

```bash
cmake --preset windows-vulkan
cmake --build --preset windows-vulkan
```

- Run from the UCRT64 shell (the executable uses its DLLs), with `ffmpeg.exe`
  and `ffprobe.exe` on `PATH` for the movies:

```bash
export PS3_VFS_ROOT="$(cygpath -m "$PWD")/game" PS3_GAMEDATA_ROOT="$(cygpath -m "$PWD")/gamedata" \
       PS3_DEV_FLASH="$(cygpath -m "$PWD")/firmware/dev_flash" \
       PS3RECOMP_VK_GUEST_PROGRAMS=1 PS3RECOMP_VK_WINDOW=1 PORT_GCM_EXPAND_BUFFERS=0
./build-windows/kamenriderbattridewarsport.exe "$(cygpath -m "$PWD")/game/EBOOT.ELF"
```

Paths are passed with `/`: the runner derives the VFS root from the EBOOT path.
Do not stop the game during its first data installation: the half-installed
data then fails with `BROKEN_EXIT_GAMEDATA` (the title screen, and nothing
more); delete `game/game/BLJS10220DATA` and start again. The launcher does this
itself: the game writes `nativePS/system/BLJS10220DATA.start` there before
installing and `.installed` after, and a folder without `.installed` is removed
before the game starts.

**PGO and the benchmark:** `scripts/pgo_train.py` and `scripts/bench_combat.py`
run on Windows from the UCRT64 shell, with ffmpeg on `PATH` (no per-thread CPU,
`--pin` or `--stacks` there: they read `/proc`):

```bash
cmake --preset windows-vulkan -B build-pgo -DPORT_PGO=generate
cmake --build build-pgo -j 2
python scripts/pgo_train.py --build build-pgo
cmake --preset windows-vulkan -B build-pgo -DPORT_PGO=use
cmake --build build-pgo -j 3
python scripts/bench_combat.py --binary build-pgo/kamenriderbattridewarsport.exe --runs 3
```

A training session ends with Ctrl+Break to the game's process group, which the
training build takes as `SIGTERM` (`SIGBREAK` in `main.cpp`). MinGW GCC
compares `-fprofile-prefix-path` with the working directory as Windows spells
it (`C:\...`), so CMake passes the build directory in that form there; with `/`
the whole absolute path stayed in the `.gcda` names (`pgo/C~/Users/...`) and
the profiles applied only in the directory that made them. Pass them to
`scripts/build_windows_package.py --pgo` to ship them; the launcher's build
then uses them.

## What was changed, and why

| Problem on Windows | Change |
|---|---|
| `PS3RECOMP_RSX_VULKAN` was Linux-only; SDL2 was linked only off Windows | SDK patch 0002: Vulkan on Windows, SDL2 pad and audio backends, pthreads |
| The D3D12 draw path linked into a Vulkan build | `rsx_live_draw.c` stubs, as off Windows (patch 0002) |
| `cellPad.c` called the XInput-only keyboard poll on every Windows build | Guarded by `PAD_BACKEND_XINPUT` (patch 0002) |
| `main.cpp` picked D3D12 on any Windows build | D3D12 only without `PS3RECOMP_RSX_VULKAN` |
| `dlopen("libvulkan.so.1")` | `vulkan-1.dll` through SDL's loader |
| SAIL decoder over `posix_spawn`, `poll`, `waitpid` | `CreateProcess`, anonymous pipes and `PeekNamedPipe`; the program is looked up on `PATH` only, as `posix_spawnp` does |
| `pread` | `ReadFile` at an offset, restoring the file position |
| MSVC `__try`/`__except` in the sampler | `ReadProcessMemory` under MinGW |
| Every other audio block silent: WASAPI drains SDL's queue a device period (about two blocks) at a time, so the mixer ran in bursts and the guest woke for one block of two | The port's mixer (`audio_bridge.c`) on Windows too, spaced by the clock |
| No sound in most boots, and the opening movie never ended (a black screen): `cellSurMixerStart` reads the notify queue key that `_cellsurMixerMain` stores, with nothing ordering the two, and the Windows thread often started too late, so key 0 was registered | Patch 0002: the creator of `_cellsurMixerMain` waits until it first blocks (at most 1 s) |
| Controllers gave only their triggers: SDL's RawInput driver gets buttons and sticks as window messages it did not receive here | `SDL_JOYSTICK_RAWINPUT=0` (XInput driver) unless set |
| Lifted chunks exceed COFF's section limit | `-Wa,-mbig-obj` |

## Stages

### 0. Setup: done

MSYS2 UCRT64 on the reference laptop (no Wine cross-build was needed).

### 1. Configure and link: done, except

- [ ] Link libstdc++ and winpthread statically, or ship their DLLs, so the
  executable runs outside the UCRT64 shell.

### 2. First boot: done, except

- [ ] Audit `long` and `off_t` (32-bit on Windows) in the bridges; `pread` was
  the only one found so far.

### 3. Playable: done, except

- [x] Data installation (21,107 files identical to the disc), movies (played,
  ended, skipped; 6,533,120 audio samples per channel as in `README_SAIL.md`),
  saves, keyboard and mouse, XInput controller, display modes, FSR, internal
  resolution.
- [x] STG02 completed by hand.
- [ ] A session longer than 30 minutes (the longest so far: 13).
- [ ] Exclusive fullscreen (only borderless was exercised).

### 4. Performance: mostly done

- [x] `store_wrap.c`, `port_ppu_memory.h` and `ppu_native.cpp` on MinGW (GNU ld
  takes `--wrap` and the weak alias). Three pairs: 43.8 against 41.0 fps, worst
  5 s 34.4 against 31.2. `PORT_PPU_NATIVE=check`: two million calls, no mismatch.
- [x] PGO trained on Windows (table above).
- [x] Thread pinning ported (`SetThreadAffinityMask`), but **opt-in**
  (`PORT_THREAD_PIN=1`): no clear gain over the Windows scheduler in three pairs.
- [x] Buffered logging measured and rejected: no difference.
- [ ] A hitch of about one second in the most crowded fight seen so far (945 ms
  at 1,027 draws), cause not found; shader compilation on first use is a
  suspect, not confirmed.
- [ ] The heaviest fights, 38-44 fps against about 45 on Linux.
- [x] `bench_combat.py` and `pgo_train.py` run on Windows (one benchmark run
  and one training session checked).
- [ ] The `PORT_PROFILE` sampler and huge pages (`SeLockMemoryPrivilege`) are
  not ported.

### 5. Launcher and importer: done

The launcher keeps its GTK 4 / libadwaita interface (MSYS2's `gtk4`,
`libadwaita` and `python-gobject`). `scripts/build_windows_package.py` assembles
the AppImage's layout for Windows, and the launcher, run from that package,
imported the ISO and built the game end to end (42 minutes, 2 jobs); the built
game then started with a plain Windows `PATH`. `tests/test_importer.py` passes
there.

- [x] RPCS3 for Windows (`packaging/rpcs3-windows.json`): downloaded and
  SHA-256 checked, unpacked with 7-Zip, run portable (its `dev_flash` beside
  `rpcs3.exe`).
- [x] `7z.exe`, `ffmpeg.exe` and `ffprobe.exe` from the package; 7-Zip lists
  disc paths with `\`, turned back into `/`.
- [x] CPU check and identity via `IsProcessorFeaturePresent` and the registry;
  build jobs from `GlobalMemoryStatusEx`.
- [x] Data in `%LOCALAPPDATA%\battride-war-port`, settings in
  `%APPDATA%\battride-war-port`; the file manager is Explorer.
- [x] Tools in a process group of their own without a console window; Stop
  ends the whole tree (`taskkill /T`). No `patchelf`: the toolchain DLLs the game
  imports (found with `objdump -p`, never `vulkan-1.dll`) are copied beside
  `runner\game-runner.exe`.
- [x] `graphics-probe.exe`.
- [x] Paths: without long path support, Windows stops at 260 characters and the
  build nests about 155 below the data folder (the SPU generator's stage folders
  carry a 64-character hash), so the data folder may have at most 100: the
  launcher says so before starting, and its temporary folders have short names.
- [x] The build identity hashes the sources' paths with `/`, so MSYS2's Python
  and the Windows one agree and a built game is not rebuilt for nothing.
- [x] Self-contained package: `build_windows_package.py --bundle` copies the
  MSYS2 UCRT64 packages the build and the launcher need (compilers, CMake, Ninja,
  SDL2, glslang, Python with capstone, pycryptodome and PyGObject, GTK 4,
  libadwaita and its icons) with every dependency pacman lists, without
  documentation and locales, into `usr/toolchain/ucrt64`: 125 packages, 27,002
  files, 2.4 GB in all. With a plain Windows `PATH` the launcher loaded no DLL
  from `C:\msys64`, `--check` passed, and Install & Play imported, built and
  started the game. Without `--bundle`, `--python` points the package at this
  computer's MSYS2.
- [x] An interrupted first data installation is cleared before the next
  start (above).
- [ ] Stop during the import, with the bundled package.
- [x] PGO profiles in the package (`--pgo`), as in the AppImage: flat names,
  the SDK's path replaced by a neutral one that the importer renames to the
  player's SDK copy (`C:` as `C_`, as CMake names the objects).
- [ ] MangoHud and GameMode are hidden (Linux tools).

### 6. Packaging and licences: not started

- Portable zip or installer (Inno Setup) of the `--bundle` package; 2.4 GB
  unpacked, so trimming (clang or gcc for C only, unused GTK modules) is worth
  measuring.
- Sources tarball for every bundled binary (MinGW runtime, winpthreads, SDL2,
  FFmpeg, 7-Zip); update `THIRD_PARTY_NOTICES.md` and `PUBLIC_FILES.txt`.
- User-facing Windows instructions, including SmartScreen, Defender and **Smart
  App Control**, which blocked a freshly built, unsigned executable once
  ("An Application Control policy has blocked this file") before allowing it.

### 7. Test matrix and beta: not started

- Windows 10 and 11; NVIDIA, AMD and Intel GPUs; dual-GPU laptops
  (`PS3RECOMP_VK_DEVICE`); build time on the player's computer.
- GitHub Actions (windows-latest + MSYS2) for the SDK and its tests only: the
  lifted code comes from the game and is not public.
- Milestone: Windows beta release.

## Main risks

- The Linux build of this branch has not been run.
- The `_cellsurMixerMain` ordering is not guaranteed on Linux either; it has not
  been seen to fail there, and the Linux code was left unchanged.
- Size of the bundled toolchain and build time with Defender active.
