# Report of November 14, 2026: what Linux needs before `windows` merges into `main`

## Summary

- **The merge itself is simple.** `main` has not moved since `windows` left it (both start from `b03edd5`), so it is a fast-forward of 12 commits: no conflicts.
- **The risk is Linux behaviour, not the merge.** Nothing on the `windows` branch has been built or run on Linux. Every Windows change was written behind `_WIN32`, `WIN32` or `__MINGW32__` and checked by reading the diff, which is what this report goes through again; a few changes deliberately reach Linux too (table in section 2).
- **Before merging, on the Linux machine:** build the branch, run the tests, play the opening and fight, compare performance with `main`, rename the PGO profiles, and rebuild and test the AppImage. Sections 1 to 7 are in that order, each with what to check; section 8 merges.

Expected time: about 2 hours, most of it compiling (the game twice for the A/B, then the AppImage build and its import).

## 1. Prepare the checkout

```sh
cd battride-war-recomp
git fetch origin
git switch windows            # or: git switch -c windows origin/windows
git status                    # clean; untracked game/, firmware/, gamedata/, src/recomp/, spu/ stay ignored
```

**SDK:** the pinned ps3recomp checkout (`../ps3recomp`, commit `a6d76c8`) already carries `0001-spu-helpers-sse41-byte-rotates.patch`. Add the second patch, which the AppImage build applies anyway:

```sh
git -C ../ps3recomp apply --check ../battride-war-recomp/patches/ps3recomp/0002-vulkan-backend-on-windows.patch
git -C ../ps3recomp apply ../battride-war-recomp/patches/ps3recomp/0002-vulkan-backend-on-windows.patch
```

It changes the SDK only for Windows (section 2). CMake checks for patch 0001 only, so a Linux build also works without 0002; apply it so the local build matches the AppImage's.

**The layout moved** (commit `0840197`): the sources are in `port/{core,graphics,audio,spu,io}/`, the scripts in `scripts/{build,perf,release}/`, `run-vulkan.sh` in `scripts/`, the per-platform packaging in `packaging/{linux,windows}/`, and `README_SAIL.md`/`README_FIRMWARE.md` became `docs/SAIL.md`/`docs/FIRMWARE.md`. The README's "Components" section has a table. Commands from before need the new paths, for example `./scripts/run-vulkan.sh` and `python3 scripts/build/generate.py`.

## 2. What reaches Linux

| Change | Where | Effect on Linux | Check |
|---|---|---|---|
| Sources moved into `port/`, CMake through `PORT_SRC` | `CMakeLists.txt` | Same files and flags; new object paths, so a full rebuild of the port's files | Section 3 builds |
| `fsr1_bridge.c` includes `../../third_party/fsr1/...` | `port/graphics/` | Same headers, found relative to the file | Section 3 builds |
| CTest block (`PORT_TESTS`, on by default) | `CMakeLists.txt` | `ctest` runs 13 tests; `find_package(Python3)` and `find_program(bash)` are optional | Section 4 |
| `-fprofile-prefix-path` through `PORT_PGO_PREFIX` | `CMakeLists.txt` | Same value on Linux; only Windows converts it to `C:\...` | Section 6 |
| Profiles are named after object paths | `scripts/perf/move_profiles.py` | Profiles trained before the move miss their 6 port C++ files until renamed | Section 6 |
| Launcher: **Graphics card** setting | `packaging/launcher.py`, `importer.py` | Runs `graphics-probe` once when the launcher opens, to list GPUs; the choice becomes `PS3RECOMP_VK_DEVICE` | Section 7 |
| Launcher: interrupted first data installation cleared | `importer.clear_interrupted_install` | Removes `game/game/BLJS10220DATA` when it lacks `BLJS10220DATA.installed`, before the game starts | Section 7 |
| Importer: `build_runner` uses `scripts/build/generate.py` and `packaging/linux/glslang-compat` | `packaging/importer.py` | New paths inside the AppImage | Section 7 |
| `place_profiles`: a rename that fails drops that profile instead of stopping the build | `packaging/importer.py` | Only on an error that stopped the build before | none |
| `pgo_train.py`: `sdk_dir` falls back to `ps3recomp_runtime_SOURCE_DIR` | `scripts/perf/` | Same result when `PS3RECOMP_DIR` is cached | Section 6 |
| `build_fsr1_shaders.py` writes LF explicitly | `scripts/build/` | None on Linux | none |
| The build identity hashes `PUBLIC_FILES.txt`'s files | `packaging/importer.py` | The file list changed, so players' games rebuild once | Expected |

**Windows-only, no Linux effect expected** (each is behind a Windows guard; worth a glance at `git diff origin/main windows` if anything in sections 3-5 misbehaves):

- `port/audio/audio_bridge.c`: `_beginthreadex` hook and the WASAPI clock spacing, `#ifdef _WIN32`; the POSIX mixer path is unchanged.
- `port/core/main.cpp`: D3D12 only without `PS3RECOMP_RSX_VULKAN` (Linux had neither); `SIGBREAK` under `#ifdef SIGBREAK`; `SDL_JOYSTICK_RAWINPUT` inside `#ifdef _WIN32`.
- `port/core/loader_bridge.cpp`, `stubs.cpp`, `thread_affinity.c`, `port/io/fs_bridge.cpp`, `port/audio/sail_decoder.hpp`, `port/graphics/vulkan_bridge.c`, `include/port_ppu_memory.h`: `_WIN32`/`__MINGW32__`/`_MSC_VER` branches; the Linux text is unchanged.
- SDK patch 0002: `if(APPLE)` instead of `if(WIN32 OR APPLE)`, SDL2 and threads inside `if(WIN32)`, `if(UNIX OR PS3RECOMP_RSX_VULKAN)` (already true on Linux), `#if PAD_BACKEND_XINPUT` (0 on Linux, as `_WIN32` was), `rsx_live_draw.c` stubs as before, and the `_cellsurMixerMain` wait only under `_WIN32 && PS3RECOMP_RSX_VULKAN`.
- `CMakeLists.txt`: `-Wa,-mbig-obj` only `if(WIN32 AND NOT MSVC)`; the fast-memory, native and store-wrap conditions became `(NOT WIN32 OR MINGW)`, which is true on Linux as `NOT WIN32` was.
- Scripts: `WINDOWS` branches in `bench_combat.py`, `pgo_train.py` and the importer (process groups, `taskkill`, CPU detection, short temporary names, path limits).

**A known unguarded difference:** none was found. If the build or the game differs from `main`, that is a finding: note what and where.

## 3. Build

The object paths changed, so start from a fresh build directory (or let Ninja rebuild; both work):

```sh
cmake --preset linux-vulkan
cmake --build --preset linux-vulkan -j2
```

- [ ] Configure prints the usual SDK and lifted-output lines and no new warning.
- [ ] The build finishes. New warnings in `port/` files are worth a look; the SDK's `-Wunused` ones are old.
- [ ] `build-vulkan/kamenriderbattridewarsport` exists.

## 4. Tests

```sh
ctest --test-dir build-vulkan -L python
ctest --test-dir build-vulkan -L unit
ctest --test-dir build-vulkan -L game      # needs build-vulkan's runtime library and the imported movie
```

- [ ] `python`: 3 tests (importer, publication, frame report). On Windows they pass, with the symlink test skipped; on Linux it should run and pass.
- [ ] `unit`: 8 shell tests (`gcm_jump`, `graphics_buffers`, `input_bridge`, `lwcond`, `ppu_memory`, `vertex_decode`, `vertex_remap`, `vmx_simd`). Only `ppu_memory` and `vmx_simd` ran on Windows (the others use Linux APIs), so these are their first run since their include paths moved to `port/`.
- [ ] `game`: `sail` and `sail_audio` (`sail_audio` expects 6,533,120 samples per channel, `docs/SAIL.md`).

## 5. Play

```sh
./scripts/run-vulkan.sh
```

- [ ] Boot, the opening movie with sound to its end (and skipping it on a second run).
- [ ] Menus, controller and keyboard and mouse.
- [ ] STG02: the spotlight scene, then a fight. `PORT_AUDIO_STATS=1 ./scripts/run-vulkan.sh` prints empty-block counts every 5 s; they should stay near 0 as on `main`.
- [ ] Closing the window ends the game.

## 6. Performance and the PGO profiles

**Rename the profiles once** (the AppImage's, trained before the move):

```sh
python3 scripts/perf/move_profiles.py build-pgo11/pgo      # "Renamed 6 profiles"; run again: 0
```

They were trained with the AppImage's compiler (GCC 11), so only a build with that toolchain applies them: section 7 checks that they are found. A `linux-vulkan` build with the system GCC discards them for the version mismatch, so do not judge them there.

**Compare with `main`** without PGO, so both builds use the same compiler and options, alternating them as for every performance change (`docs/PERFORMANCE.md`):

```sh
git worktree add ../bw-main origin/main     # main's sources; give it the same game/, firmware/, gamedata/, src/recomp/, spu/
(cd ../bw-main && cmake --preset linux-vulkan && cmake --build --preset linux-vulkan -j2)
python3 scripts/perf/bench_combat.py --binary ../bw-main/build-vulkan/kamenriderbattridewarsport --runs 1 --label main
python3 scripts/perf/bench_combat.py --binary build-vulkan/kamenriderbattridewarsport --runs 1 --label windows
# three such pairs in all
```

- [ ] Averages within run-to-run noise of each other (about 1.5 fps either way). For reference, the Linux PGO build ran at 51.3 fps average, 47.0 worst 5 s (`docs/PERFORMANCE.md`); without PGO it is lower.

## 7. The AppImage

The build scripts moved, so the AppImage build is the step most likely to need a fix. In the Ubuntu 22.04 build container (`packaging/linux/build-image/Containerfile`), as in `docs/APPIMAGE.md`:

```sh
python3 scripts/release/build_appimage.py ... --pgo build-pgo11/pgo --output dist/BattrideWarRecomp-v<next>-x86_64.AppImage
python3 scripts/release/collect_sources.py --stage <AppDir> ...
```

- [ ] The build finds `packaging/linux/rpcs3.json`, `packaging/graphics_probe.c` and the renamed profiles, and refuses nothing.
- [ ] `./BattrideWarRecomp-*.AppImage --check` passes.
- [ ] `--import-only <iso> --data-dir <new folder>`: import, translation (`scripts/build/generate.py` inside the AppImage) and compilation finish.
- [ ] The renamed profiles apply: in `<new folder>/build.log`, `grep "profile count data file not found" <new folder>/build.log | grep -c "port#"` gives 0. On Windows the same check found all six (`build-pgo/pgo-win`). `coverage-mismatch` warnings are expected for files changed after training.
- [ ] Launcher: it opens without a noticeable delay (it now runs `graphics-probe` once to list GPUs); **Settings, Rendering, Graphics card** lists the GPUs by name; choosing one starts the game on it (`using device N` in `game.log`). On a laptop with two GPUs, Automatic still picks the dedicated one.
- [ ] Interrupted installation: close the game during its first data installation, start again: the launcher says the installation was interrupted, and the game installs again instead of stopping at the title screen with `BROKEN_EXIT_GAMEDATA`.
- [ ] Stop during the ISO extraction leaves no `.import-*` folder, and the next import completes.
- [ ] An existing data folder from 0.5.2 rebuilds the game once (the build identity changed) and keeps its saves.

## 8. Merge

When sections 3 to 7 pass, or their failures are fixed on `windows`:

```sh
git switch main
git merge --ff-only windows
git push origin main
```

Then, separately: the release (version number, release notes covering the Windows package, the AppImage and the Windows zip with their source tars), and deciding whether to keep developing on `windows` or to delete it.

## Reference

- Commits on `windows` not in `main`: `5f5eab2` to `e387460` (`git log --oneline origin/main..windows`).
- Windows checks already done, for comparison: the game built with the new layout ran the STG02 benchmark at 43.6 fps without PGO (41.9 before the move); CTest passes the 5 tests registered there; the six port profiles were found after renaming (`docs/WINDOWS.md`).
