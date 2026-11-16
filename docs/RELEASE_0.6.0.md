# Release 0.6.0 plan: Linux and Windows

Version 0.6.0 is the first release with both packages: the Linux AppImage and
the Windows zip. It also carries the performance work below. **It is published
only when both packages are built and tested**; until then the GitHub release
stays a draft.

Who does what: **Linux** steps run on the Linux test laptop (Ryzen 5 4600H,
GTX 1650 and integrated Radeon RENOIR). **Windows** steps, and anything that
needs a person at the screen (launcher GUI, closing the window, controllers),
are done by the developer on Windows 11, same laptop.

## Where things stand (November 16, 2026)

- [x] `windows` merged into `main` (fast-forward, `1302673`); Linux checks of
      [reports/2026-11-14-linux-merge.md](reports/2026-11-14-linux-merge.md)
      passed: build, 13 CTest tests, opening movie, STG02, A/B on the
      integrated GPU (`main` 47.4 fps, `windows` 51.4), AppImage import and
      launcher logic.
- [x] `cc96fa6`: the window is titled **Battride War**, app_id/WM_CLASS
      `battride-recomp` (checked with `WAYLAND_DEBUG`).
- [x] `5f4c570`: version 0.6.0 in `build_appimage.py`,
      `build_windows_package.py`, README, APPIMAGE.md, WINDOWS_PACKAGE.md.
- [ ] Push `main`: the commits since `1302673` stay local until the release
      is published (the README already announces 0.6.0).
- [x] Leftovers removed (`../bw-test-0.6.0`, the partial sources folder). The
      AppImage built on November 15 is superseded by any change below and is
      rebuilt in step 5.
- [x] The unpushed commits are on the branch `release-0.6.0` (November 16),
      so the Windows side can fetch them; `main` on GitHub is unchanged.

## What is left, in order (November 16, 2026)

Steps 1-4 and 5-6 are done on Linux except for the manual tests. This is the
rest, in the order to do it; details in steps 4-7 below.

**Windows: steps 0-7 done on November 16** (results in step 4, step 5 and the
Windows column of step 6), except the tests that need a person at the screen.
The training tree was `C:/pgo060m`, not `C:/pgo060`: the first tree there had
been made without the MP3 codec (see step 4) and could not be removed from
that session. The commands, for the next time:

0. Get the code: `git fetch origin && git checkout release-0.6.0`.
1. A stage for training only, without profiles:
   ```bash
   python scripts/release/build_windows_package.py --stage ../stage-train \
       --sevenzip-dir "C:/Program Files/7-Zip" --bundle
   ```
2. The training tree in the importer's layout:
   ```bash
   python scripts/perf/pgo_tree.py --stage ../stage-train --root C:/pgo060
   ```
3. Train (~3 h): the `cmake` and `pgo_train.py` commands of
   [WINDOWS.md](WINDOWS.md) ("PGO and the benchmark"), with
   `<stage>` = `../stage-train`. The profiles end up in `C:/pgo060/pgo`.
4. Rebuild with `-DPORT_PGO=use` and run `bench_combat.py --runs 3` on each
   GPU (`PS3RECOMP_VK_DEVICE=0` integrated, `1` the GTX 1650), against a build
   with the old profiles (`build-pgo/pgo-win`).
5. The release package, in a new stage, with the new profiles:
   ```bash
   python scripts/release/build_windows_package.py --stage ../BattrideWarRecomp \
       --sevenzip-dir "C:/Program Files/7-Zip" --bundle --pgo C:/pgo060/pgo
   python scripts/release/collect_windows_sources.py --package ../BattrideWarRecomp \
       --out ../BattrideWarRecomp-v0.6.0-windows-sources.tar
   "/c/Program Files/7-Zip/7z.exe" a -tzip -mx=5 BattrideWarRecomp-v0.6.0-windows.zip ../BattrideWarRecomp
   sha256sum BattrideWarRecomp-v0.6.0-windows.zip > BattrideWarRecomp-v0.6.0-windows.zip.sha256
   sha256sum ../BattrideWarRecomp-v0.6.0-windows-sources.tar > ../BattrideWarRecomp-v0.6.0-windows-sources.tar.sha256
   ```
6. The Windows column of step 6, from the zip extracted to a short folder
   (`C:\Games`); also whether the pipeline cache removed the 945 ms hitch.
7. Any fix committed on Windows: `git push origin release-0.6.0`.

**Linux, by hand:** closing the window; controller, keyboard and mouse; the
30-minute session (step 6).

**Then, step 7:** the report in `docs/reports/` and the docs with the Windows
numbers; rebuild the AppImage and the Linux sources tar if a shipped file
changed (both packages come from the same final commit); `git push origin
main`, tag `v0.6.0`, draft release with the eight files; publish when both
sides are tested; delete the `release-0.6.0` branch and the test folders.

## 1. Baseline measurements

Before changing code: where the time goes today, per platform and GPU. An
item in step 2 whose cost is under 3% of its thread is dropped.

**Linux**

- [x] `PORT_PROFILE` CPU samples in STG02 fighting, integrated and GTX 1650:
      shares of `runTaskMain` and of the render thread (FIFO walk, vertices,
      texture hashes, Vulkan).
- [x] Sensors during `bench_combat.py` on each GPU: `gpu_busy_percent`
      (`/sys/class/drm/card1/device`), per-core clocks, APU power and
      temperature (`amdgpu`, `k10temp` hwmon). Answers whether the integrated
      GPU is limited by the GPU, the CPU, or the APU's shared power budget.
- [ ] A repeatable heavy fight (1,000+ draws) from the full-story save. STG02
      already reaches 786-794 draws, inside the 650-880 range of the
      documented drops; the 1,027-draw case is still to reproduce.
- Caveat: the laptop boots on HPET, which distorts per-draw timing
  (`PORT_ENG_PROFILE`). Booting with `tsc=reliable` fixes that for
  measurement only; it is a system change that needs the developer's consent.

**Windows**

- [x] `bench_combat.py --runs 3` on each GPU (step 4, November 16).
- [ ] A Windows Performance Recorder capture of a fight (`PORT_PROFILE` is not
      available on Windows, `port/core/stubs.cpp:376`).

**Output:** a cost table per item, added to this file.

### Results on Linux (November 15, 2026)

Build `build-vulkan` at `98e2af4`, no PGO, STG02, one 60 s fight per run with
`bench_combat.py`; sensors every 0.5 s; `PORT_PROFILE` at 1000 Hz in separate
runs (about 60,000 fight samples each).

| | Integrated (RADV RENOIR) | GTX 1650 |
|---|---|---|
| Average fps (plain / with profiler) | 48.4 / 51.7 | 51.3 / 52.5 |
| Worst 5 s | 40.0 / 39.8 | 41.8 / 41.8 |
| p95 frame | 26.2 ms | 24.0 ms |
| Frames over 50 ms in the fight | 0 | 0-2 (longest 70 ms) |
| `runTaskMain` | 93% of a core | 92-93% |
| Render thread (FIFO, vertices, Vulkan) | 45% | 45% |
| Integrated GPU busy | 31% average, 51% peak | 4% (idle) |
| APU power; fastest core | 30 W average (53 W peak); 4.0 GHz throughout | 26 W; 4.0 GHz |

**Conclusions:**

- **The limit is the CPU, in `runTaskMain`, on both GPUs.** The integrated
  GPU is not the bottleneck on this laptop (31% busy) and there is no power
  or thermal throttling (the fastest core holds its 4.0 GHz boost on AC).
- **The frame limiter does not spin in a fight below 60 fps:** clock reads
  and `func_003ABE20` are 0.1% of `runTaskMain`. The 14% measured on
  October 28 was the game capped at 60 fps.
- **The render thread is not the limit** (45% of a core), so render-side
  savings would not raise the average.
- **No pipeline is created during the fight**; the 230-620 ms frames are
  scene loads before it.

Where `runTaskMain`'s time goes (integrated / GTX):

| Share | What |
|---|---|
| 16-17% | Guest memory helpers (`vm_read32_inl` 8.8%, stores 5.7%, `vm_raw_ld16` 1.3%), called from chunks without the fast path; largest callers `func_005AAC60` (chunk 020, 1.6-2.9%) and `func_003C7A98` (chunk 013, 1.3-1.5%), the rest spread |
| 14% / 11-12% | Translated code in chunk 021 / chunk 015, neither built at `-O3` (only 029 is) |
| 5.4-5.9% | `native_004B5120`, the list lookup |
| 5.6%, 2.9%, 2.1%, 1.7% | Hottest guest functions: `func_005C79B4` (021), `func_0042D188` (015), `func_005C8CE0` (021), `func_005C82BC` (021) |
| 0.1% | Frame limiter and clock reads |

Render thread, for reference: vertex fetch and remap 22%, FIFO walk and
method dispatch 28%, texture hashes 8%.

## 2. Performance work, by importance

Reordered after the step 1 results: only `runTaskMain` limits the frame
rate, so the items that make it cheaper come first.

Rules for every item: three interleaved A/B pairs of `bench_combat.py` (60 s
of fighting, no PGO on either side; the average spreads about ±3 fps, so worst
5 s and p95 weigh more); a captured frame compared when rendering changes; the
13 CTest tests; one commit per item. Anything that measures worse is reverted
and recorded in [PERFORMANCE.md](PERFORMANCE.md).

| # | Item | Measured cost | Where |
|---|---|---|---|
| 1 | ~~**`-O3` on chunks 021 and 015**~~ **Rejected November 15:** 48.8 against 50.3 fps, worst 5 s 39.8 against 39.9, p95 26.1 against 25.6 ms over three pairs on the integrated GPU ([PERFORMANCE.md](PERFORMANCE.md)). | 25% of `runTaskMain` | `CMakeLists.txt` (`PORT_HOT_PPU_O3`) |
| 2 | ~~**Guarded fast memory path for the top caller functions**~~ **Rejected November 15:** wrapping `func_005AAC60` and `func_003C7A98` measured 49.1 against 48.2 fps (noise); profiled, the two functions with their helpers went from 6.05% to 5.65% of `runTaskMain`, as the helpers' cost is their checks and the access, not the call ([PERFORMANCE.md](PERFORMANCE.md)). | up to 4.4% | `scripts/build/generate.py`, `include/port_ppu_memory.h` |
| 3 | **The pending inline change in `include/port_ppu_memory.h`. Done November 15** with the PGO retrain of step 4. | part of the 16-17% | `include/port_ppu_memory.h` |
| 4 | **Pipeline cache on disk. Done November 15:** one `VkPipelineCache` for all four `vkCreate*Pipelines` calls, kept in `~/.cache/battride-war-port/` or `%LOCALAPPDATA%\battride-war-port\`, one file per GPU. Time spent creating the 198 pipelines up to the first fight: GTX 1650 1,548 ms with no cache, 113 ms with only the driver's cache (as before), 13 ms with ours; integrated Radeon 462 / 8.5 / 4.5 ms. No fps change in the fight. Still to check on Windows, and whether it was the 945 ms hitch at 1,027 draws ([WINDOWS.md](WINDOWS.md)) ([PERFORMANCE.md](PERFORMANCE.md)). | loads; hitch | `port/graphics/vulkan_bridge.c` |

**Dropped for 0.6.0** (under 3% of the limiting thread, or not on it): sleep
in the frame limiter and the TSC timebase (0.1% in a fight), the main-thread
poll loop (guest main thread at 31%), FIFO dispatched once, vectorised texture
hash, fewer barriers (render thread not the limit), internal scale below 1
(integrated GPU 31% busy). They stay candidates for weaker GPUs or for the
game capped at 60 fps, if a measurement there shows their cost.

**Moved to 0.6.1** (each needs days of verification): a key-to-entry cache for
the `func_004B5120` list lookup (5.4-5.9%), native rewrites of the hottest
guest functions (`func_005C79B4` 5.6% first), skipping texture hashes for
unwritten pages, and reusing converted vertices across frames.

## 3. Launcher fixes

- [x] The GPU list leaves out software renderers (`llvmpipe`, `lavapipe`,
      `softpipe`, SwiftShader); the game's device numbers still count them.
- [x] Ctrl+C, SIGTERM or SIGHUP during `--import-only` stops the running tool
      (its session is killed) and removes `.import-*` or the build folder;
      exit code 130. Tested November 15 with the signal sent while 7-Zip, RPCS3
      (`--decrypt`, `--installfw`) and the build ran: nothing left but the
      logs, no tool process left (an import already verified is kept).
- [x] The `tools/` folder (unpacked RPCS3) is removed after **Stop** and
      Ctrl+C too (tested through `Runner.stop()`); RPCS3 is copied or
      downloaded again on the next import.
- [x] **Decided November 15: update phase A moves to 0.6.1** (drop the package
      version from the build identity and keep the build folder between
      versions). 0.6.1 still recompiles everything once; later updates then
      recompile only what changed.

## 4. PGO profiles, last

Profiles are specific to each compiler, so there are two sets, retrained
after every code change above.

- [x] **Linux** (the AppImage's GCC 11), November 15: the inline stores of
      `include/port_ppu_memory.h` now use the `port_ppu_store_end` gate, and
      the profiles were retrained on that code (`build-pgo11/pgo`, 93 files;
      the previous ones in `build-pgo11/pgo-1028-port`). Three interleaved
      pairs on the integrated GPU: 59.0 against 58.3 fps, worst 5 s 54.3
      against 53.0 (better in every pair), p95 19.5 against 19.7 ms, frames
      over 16.7 ms 1,831 against 1,956 ([PERFORMANCE.md](PERFORMANCE.md)).
- [x] **Linux, the player's build** (November 15): the game the AppImage built
      got only part of the profiles (57.9 against 58.9 fps, worst 5 s 50.4
      against 53.7), because GCC hashes the source file names. PGO builds now
      compile with relative paths (`scripts/build/pgo_relpaths.py`) and the
      profiles are trained in the importer's layout
      (`scripts/perf/pgo_tree.py`); the AppImage's own build then measured
      58.7 against 58.5 fps, worst 5 s 54.3 against 54.0, in three pairs.
- [x] **Windows** (MSYS2 GCC 16.2.0), November 16: trained in the importer's
      layout (`C:/pgo060m`, two sessions, 93 profiles in `C:/pgo060m/pgo`).
      Built with them, the tree reported no missing profile and 2 mismatches;
      with the old ones (`build-pgo/pgo-win`, the repository layout) 209
      mismatches and the two SDK files missing. Three interleaved pairs per
      GPU, STG02, 60 s, better in all six:

      | | Old profiles | New profiles |
      |---|---|---|
      | Integrated Radeon: average / worst 5 s / p95 | 47.9 fps / 40.9 / 26.7 ms | 56.5 fps / 48.7 / 21.8 ms |
      | GTX 1650: average / worst 5 s / p95 | 54.0 fps / 43.3 / 24.4 ms | 58.4 fps / 53.3 / 20.6 ms |

- [ ] **`pgo_tree.py` translated the audio task without its MP3 codec**: it
      did not pass the firmware to `generate.py`, while the importer does
      (`PS3_DEV_FLASH`), so the training ran the codec interpreted while the
      player's game runs it translated. Fixed in `de9b05c` (`--dev-flash`,
      default `firmware/dev_flash`); the Windows training above used the fix
      ("mp3: 366 entries translated"). **Linux:** the November 15 profiles
      were probably trained the same way (check the training tree's
      `generate.py` output for "flashMP3.pic: not found"). The effect is
      likely small, as PGO covers only the C++ files and the codec is C, but
      the training load differed; retraining is a decision for Linux.

## 5. Release builds

- [ ] Push `main` with everything; tag `v0.6.0` on the final commit.
- [x] **Linux, built November 15 from `1e9a6e4`, not pushed yet:**
      `dist/v0.6.0/BattrideWarRecomp-v0.6.0-x86_64.AppImage` (sha256
      `bc75d795…`) and `BattrideWarRecomp-v0.6.0-sources.tar` (`8d64e8fb…`,
      192 source packages; glibc, linux and pcre2 left the Ubuntu archive and
      come from 0.5.2's verified copies via `--reuse`). `--check` passes; a
      new import and build through it measured as the training tree. Rebuild
      both if a shipped file changes before the tag.
- [ ] **Linux** (reference): AppImage in the Ubuntu 22.04 container
      ([APPIMAGE.md](APPIMAGE.md), `--pgo build-pgo11/pgo`), sources tar
      (`collect_sources.py --runtime-commit fedfad51acc2860b2916a1eef32ba4a500ff31ef`),
      a `.sha256` for each file. The type2 runtime and 7-Zip come from the 0.5.2
      AppImage (hashes checked against its `app/package.json`).
- [x] **Windows, built November 16 from `de9b05c`, not uploaded yet**, in
      `C:\Users\qimin\Documents`: `BattrideWarRecomp-v0.6.0-windows.zip`
      (741 MB, sha256 `3abe9aad0a2038a375b6288f1fb3865d1122185f35c68b23cc63d29289e32f64`)
      and `BattrideWarRecomp-v0.6.0-windows-sources.tar` (2.28 GB, 166 source
      archives and `SOURCES.md`, sha256
      `672b19659fbb71101df01aa41aa537bd4606ce96516c3e6589d15eaf0f0a8fb7`), each
      with a `.sha256`. Version `0.6.0-windows`, `--pgo C:/pgo060m/pgo`. No
      shipped file changed since the AppImage's `1e9a6e4`, so both packages
      carry the same sources.
- [ ] **Windows** (reference): from the MSYS2 UCRT64 shell, in a checkout of the final
      `main` ([WINDOWS_PACKAGE.md](WINDOWS_PACKAGE.md)):
      `build_windows_package.py --bundle --pgo build-pgo/pgo-win`,
      `collect_windows_sources.py`, the zip
      `BattrideWarRecomp-v0.6.0-windows.zip`, the
      `BattrideWarRecomp-v0.6.0-windows-sources.tar`, a `.sha256` for each.

## 6. Release tests

| Test | Linux (AppImage) | Windows (zip) |
|---|---|---|
| `--check` / the launcher opens without a noticeable delay | [x] | [x] |
| New import through play: movie, menus, STG02 | [x] | [x] |
| Upgrade from a 0.5.2 data folder rebuilds once and keeps saves | [x] | n/a |
| GPU list in the launcher; the chosen GPU is used (`using device N`) | [x] | [x] |
| Interrupted first data installation recovers | [x] | [x] |
| **Stop** during extraction and build leaves no temporary files | [x] | [x] |
| Window, Alt+Tab and taskbar show **Battride War** | [x] | window title only |
| Closing the window ends the game; controller, keyboard and mouse | [ ] | [ ] |
| `bench_combat.py` on both GPUs against 0.5.2 | [x] | [x] (against the old profiles) |
| 30-minute session without dropouts or stalls | [ ] | [ ] |

**Criterion:** no regression against 0.5.2 on Linux, and Windows working end
to end on both GPUs.

### Linux results (November 15-16, AppImage built from `1e9a6e4`)

- **Launcher:** `--check` passes; the window is up in about 1.5 s.
- **New import through play:** an empty data folder (`build-pgo11/player-test`):
  import and build, then the launcher's Play environment (`game_env` +
  `gpu_env`, driven with scripted presses) through the movie and menus to the
  first STG02 fight in 131 s.
- **Upgrade from 0.5.2:** a data folder imported and built by the 0.5.2
  AppImage, with saves: 0.6.0 rebuilt once (11 min, `build.json` package
  0.6.0), the saves stayed byte-identical, a second start reused the build
  (1 s), and the game loaded the saves and reached the fight.
- **GPU list:** offers the Radeon and the GTX 1650, not llvmpipe; choosing
  each by name gave `using device 0` and `using device 1`; Automatic took the
  GTX.
- **Interrupted data installation:** the game killed 3 s after
  `BLJS10220DATA.start` appeared; the next start removed the half-installed
  folder, installed again (`.installed` written) and played.
- **Ctrl+C / Stop:** through the AppImage, during the ISO extraction and
  during the build: exit 130, only the logs (and the verified import, for the
  build) left, no tool process left. Stop through `Runner.stop()` in step 3.
- **Window name:** `set_title("Battride War")`, `set_app_id("battride-recomp")`
  (Wayland, GTX chosen).
- **Against 0.5.2**, the game each AppImage built, three interleaved pairs per
  GPU, STG02, 60 s:

  | | 0.5.2 | 0.6.0 |
  |---|---|---|
  | Integrated Radeon: average / worst 5 s / p95 | 54.8 fps / 46.3 / 23.0 ms | 58.5 fps / 53.9 / 19.5 ms |
  | GTX 1650: average / worst 5 s / p95 | 57.0 fps / 49.0 / 21.0 ms | 59.7 fps / 57.9 / 19.1 ms |

  Better in all six pairs; 1% low 37.1 to 41.6 and 38.8 to 44.0 fps.
- **Still to do on Linux:** closing the window, controller, keyboard and mouse
  (by hand, through the launcher); the 30-minute session (a scripted run was
  stopped early at the user's request).
- **Noted:** the game log shows a few `xkbcommon: ERROR ... dead_hamza` lines:
  the bundled libxkbcommon predates the host's Compose file; harmless.

### Windows results (November 16, the zip extracted to `C:\Games`)

- **`--check`** passes with a plain Windows `PATH`; the launcher window
  (`BattrideWarRecomp.cmd`) is up in about 3.7 s, listing the GPUs included.
- **New import through play:** `--import-only` into `C:\Games\bwdata`: import,
  translation (the MP3 codec translated) and build in about 19 min with 3 jobs;
  `tools\` removed afterwards. Every profile found, the port's files included;
  34 mismatch warnings, all in cold code: `main()` (the training build has hooks
  the final one lacks) and the SDK files `ppu_hle`, `ppu_sysprx`, `ppu_fs`,
  which the compiler sees under another absolute path than in the training
  stage. Then the movie, menus and the STG02 fight through `bench_combat.py`.
- **GPU list:** the Radeon and the GTX 1650; the launcher's Play environment
  (`game_env` + `gpu_env`, scripted) with each name gave `using device 1` and
  `using device 0`.
- **Interrupted data installation:** the game stopped 7 s after
  `BLJS10220DATA.start`; the next start cleared the folder, installed again
  (`.installed` after 208 s) and played; no `BROKEN_EXIT_GAMEDATA`.
- **Stop** (`Runner.stop()`, as the button) 8 s into the ISO extraction: nothing
  left in the data folder, `tools\` included; 30 s into the build: only the
  verified import left; no 7-Zip, RPCS3, Ninja or compiler process left.
- **Window name:** the game window's title is **Battride War** (`tasklist /v`);
  Alt+Tab and the taskbar not looked at.
- **Performance**, the game the package built, two runs per GPU: integrated
  57.4 fps average, worst 5 s 50.8, p95 21.4 ms; GTX 1650 58.4, 53.4, 20.9 ms,
  as the training tree's PGO build (step 4).
- **Pipeline cache:** one file per GPU in `%LOCALAPPDATA%\battride-war-port\`
  (`pipelines-1002-1636.bin`, `pipelines-10de-1f9d.bin`); later runs report it
  up to date, under 1 ms creating pipelines. No frame over 200 ms inside the
  fights; the longer frames are scene loads before them. The 945 ms hitch was
  in a 1,027-draw fight that STG02 does not reach, so whether the cache
  removed it is still open.
- **Still to do on Windows, by hand:** closing the window, controller, keyboard
  and mouse through the launcher; Alt+Tab and the taskbar name; the 30-minute
  session. The test data folder `C:\Games\bwdata` is imported, built and
  installed for that (choose it as the data folder in the launcher).

## 7. Publication

- [ ] A report in `docs/reports/` with the measured gain of each item per
      platform; [PERFORMANCE.md](PERFORMANCE.md), [APPIMAGE.md](APPIMAGE.md)
      ("What changed in 0.6.0"), [WINDOWS_PACKAGE.md](WINDOWS_PACKAGE.md) and
      the README updated with the results.
- [ ] GitHub release `v0.6.0` created as a **draft** with the eight files:
      the AppImage, the Linux sources tar, the Windows zip and the Windows
      sources tar, each with its `.sha256`.
- [ ] Published only when both sides are complete and tested.
- [ ] Test folders removed.

## Estimated time

| Step | Linux | Windows |
|---|---|---|
| 1. Measurements | ~1 h | ~1 h |
| 2. Performance work | 2-4 days, depending on how many items pass | ~30 min check per item |
| 3. Launcher fixes | ~2 h | n/a |
| 4. PGO | ~3 h | ~3 h |
| 5-6. Builds and tests | about 1 day for both | |
| 7. Publication | ~1 h | n/a |

## Open decisions

1. ~~Update phase A (step 3) in 0.6.0 or 0.6.1?~~ 0.6.1 (decided November 15).
2. `tsc=reliable` at boot on the test laptop, for measurement only?
3. When is the Windows machine available for steps 1, 4, 5 and 6?
