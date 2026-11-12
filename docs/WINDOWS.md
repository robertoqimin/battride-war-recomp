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

Not done: the launcher and importer, packaging, testing on other machines, and
a build of this branch on Linux (every change is behind `_WIN32`, `WIN32` or
`__MINGW32__`, and was checked by reading, not by building there).

Performance in STG02 fighting, 60 s windows of the scripted route of
`scripts/bench_combat.py`, alternating builds:

| Build | Average | p95 frame | Worst 5 seconds |
|---|---|---|---|
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
data then fails with `BROKEN_EXIT_GAMEDATA`; delete `game/game` and start again.

**PGO:** configure another build directory with `-DPORT_PGO=generate`, play
with it (the instrumented build runs at a few frames per second), and end the
game with **Ctrl+Break** in its console: the training build writes its profiles
on `SIGBREAK` as on `SIGTERM`. Then configure the same directory with
`-DPORT_PGO=use` and build again. On Windows the `.gcda` names keep the build
directory's absolute path, so profiles only apply in the directory that made
them.

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
- [ ] `bench_combat.py` and `pgo_train.py` for Windows in `scripts/` (they read
  `/proc` and send `SIGTERM`).
- [ ] The `PORT_PROFILE` sampler and huge pages (`SeLockMemoryPrivilege`) are
  not ported.

### 5. Launcher and importer: not started

- A Windows entry in `rpcs3.json` (the build above): the `.7z` runs portable, so
  `dev_flash` and the decrypted EBOOT land in different places.
- Windows `7z.exe` and `ffmpeg.exe`.
- CPU check via `IsProcessorFeaturePresent` instead of `/proc/cpuinfo`.
- Data in `%LOCALAPPDATA%\battride-war-port`, settings in `%APPDATA%`.
- Process control with `CREATE_NEW_PROCESS_GROUP` instead of signals (Ctrl+Break
  works for the PGO training build); drop the `patchelf` step.
- Port `graphics_probe.c`.
- UI: GTK4 from MSYS2 (keeps the code) or another toolkit — to decide.
- Bundle a minimal toolchain so the game is built on the player's computer; the
  runner is never shipped. Shipped PGO profiles need the build directory problem
  above solved first.

### 6. Packaging and licences: not started

- Portable zip or installer (Inno Setup).
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
- Size of the bundled toolchain and build time with Defender active (stage 5).
