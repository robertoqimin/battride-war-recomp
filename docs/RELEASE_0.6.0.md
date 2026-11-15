# Release 0.6.0 plan: Linux and Windows

Version 0.6.0 is the first release with both packages: the Linux AppImage and
the Windows zip. It also carries the performance work below. **It is published
only when both packages are built and tested**; until then the GitHub release
stays a draft.

Who does what: **Linux** steps run on the Linux test laptop (Ryzen 5 4600H,
GTX 1650 and integrated Radeon RENOIR). **Windows** steps, and anything that
needs a person at the screen (launcher GUI, closing the window, controllers),
are done by the developer on Windows 11, same laptop.

## Where things stand (November 15, 2026)

- [x] `windows` merged into `main` (fast-forward, `1302673`); Linux checks of
      [reports/2026-11-14-linux-merge.md](reports/2026-11-14-linux-merge.md)
      passed: build, 13 CTest tests, opening movie, STG02, A/B on the
      integrated GPU (`main` 47.4 fps, `windows` 51.4), AppImage import and
      launcher logic.
- [x] `cc96fa6`: the window is titled **Battride War**, app_id/WM_CLASS
      `battride-recomp` (checked with `WAYLAND_DEBUG`).
- [x] `5f4c570`: version 0.6.0 in `build_appimage.py`,
      `build_windows_package.py`, README, APPIMAGE.md, WINDOWS_PACKAGE.md.
- [ ] Push `main` (the two commits above are local).
- [ ] Remove leftovers: the test folder `../bw-test-0.6.0` and the partial
      `dist/v0.6.0/BattrideWarRecomp-v0.6.0-sources.d`. The AppImage built on
      November 15 is superseded by any change below and is rebuilt in step 5.

## 1. Baseline measurements

Before changing code: where the time goes today, per platform and GPU. An
item in step 2 whose cost is under 3% of its thread is dropped.

**Linux**

- [ ] `PORT_PROFILE` CPU samples in STG02 fighting, integrated and GTX 1650:
      shares of `runTaskMain` and of the render thread (FIFO walk, vertices,
      texture hashes, Vulkan).
- [ ] Sensors during `bench_combat.py` on each GPU: `gpu_busy_percent`
      (`/sys/class/drm/card1/device`), per-core clocks, APU power and
      temperature (`amdgpu`, `k10temp` hwmon). Answers whether the integrated
      GPU is limited by the GPU, the CPU, or the APU's shared power budget.
- [ ] A repeatable heavy fight (650-1,000+ draws) from the full-story save,
      besides STG02.
- Caveat: the laptop boots on HPET, which distorts per-draw timing
  (`PORT_ENG_PROFILE`). Booting with `tsc=reliable` fixes that for
  measurement only; it is a system change that needs the developer's consent.

**Windows**

- [ ] `bench_combat.py --runs 3` on each GPU.
- [ ] A Windows Performance Recorder capture of a fight (`PORT_PROFILE` is not
      available on Windows, `port/core/stubs.cpp:376`).

**Output:** a cost table per item, added to this file.

## 2. Performance work, by importance

Rules for every item: three interleaved A/B pairs of `bench_combat.py` (60 s
of fighting, no PGO on either side; the average spreads about ±3 fps, so worst
5 s and p95 weigh more); a captured frame compared when rendering changes; the
13 CTest tests; one commit per item. Anything that measures worse is reverted
and recorded in [PERFORMANCE.md](PERFORMANCE.md).

| # | Item | Where | Linux | Windows |
|---|---|---|---|---|
| 1 | **Pipeline cache on disk.** One `VkPipelineCache` passed to the four `vkCreate*Pipelines` calls (all pass `VK_NULL_HANDLE` today: `vulkan_bridge.c` 1193, 1534, 2237, 4072), loaded at start and saved at exit, keyed by GPU and driver; log pipeline creations per frame and their time. Candidate cause of the ~1 s hitch (945 ms at 1,027 draws, [WINDOWS.md](WINDOWS.md)). | `port/graphics/vulkan_bridge.c` | implement, A/B | same code; confirm |
| 2 | **Sleep in the frame limiter.** The game sleeps whole milliseconds, then spins on the timebase to the frame deadline (`func_003ABE20` calling `func_003C4148`, ~14% of `runTaskMain`). Sleep until ~0.5 ms before it. No fps gain expected with the GTX; on an APU the spinning core's power goes back to the other cores and the GPU. | native function, as `port/core/ppu_native.cpp` | implement, A/B | needs a high-resolution waitable timer (`Sleep` is 1-15 ms); confirm |
| 3 | **FIFO methods dispatched once.** Every method goes to the draw engine and also to the legacy `rsx_state` (`gcm_bridge.c:2445`); send the legacy path only the methods it still serves. | `port/graphics/gcm_bridge.c` | implement, A/B, compare frames | same code |
| 4 | **Timebase from the TSC** when it is invariant, calibrated against the system clock, HPET as fallback: every clock read stops being a system call on HPET machines. | `ppu_timebase_now`, SDK patch `0003` | implement, A/B, 10 min drift check | not needed (`QueryPerformanceCounter`) |
| 5 | **Main-thread poll loop** (`func_0009FB58`, `usleep(30)` repeated) waits on an event instead. | native function | if step 1 shows its cost | same |
| 6 | **Internal scale below 1 (0.75, 0.5) with FSR 1**, and a "Performance" preset suggested when the GPU is integrated. | `vulkan_bridge.c`, `packaging/launcher.py` (`RENDER_SCALES`) | if the integrated GPU is GPU-bound | same |
| 7 | **Vectorised texture change hash.** Already sampled (64 B per 2 KB per frame, full hash every 8 frames), so the gain may be small. | `port/graphics/rsx_engine_bridge.c` | if over 3% | same code |
| 8 | **Fewer barriers and render-pass breaks per draw; dynamic state** for fewer pipelines. | `vulkan_bridge.c` | after item 1, guided by its counters | same code |

**Cut for 0.6.0:** items 1-8 that pass their A/B. Every C/C++ item applies to
both platforms.

**Moved to 0.6.1** (each needs days of verification): a key-to-entry cache for
the `func_004B5120` list lookup, skipping texture hashes for unwritten pages,
and reusing converted vertices across frames.

## 3. Launcher fixes

- [ ] The GPU list leaves out software renderers (`llvmpipe`).
- [ ] Ctrl+C during `--import-only` also stops the extraction tool and
      removes `.import-*` (tools run in their own session, so the signal does
      not reach them; seen on November 14).
- [ ] The `tools/` folder (unpacked RPCS3) is removed after **Stop** too.
- [ ] **Decision:** update phase A, i.e. drop the package version from the build
      identity and keep the build folder between versions, so later updates
      recompile only what changed. If it ships in 0.6.0, 0.6.1 already
      updates in minutes; otherwise 0.6.1 still recompiles everything.

## 4. PGO profiles, last

Profiles are specific to each compiler, so there are two sets, retrained
after every code change above.

- [ ] **Linux** (the AppImage's GCC 11): apply the pending inline change in
      `include/port_ppu_memory.h`, retrain with `scripts/perf/pgo_train.py`,
      A/B against the current profiles (~3 h).
- [ ] **Windows** (MSYS2 GCC):
      `cmake --preset windows-vulkan -B build-pgo -DPORT_PGO=generate`,
      `cmake --build build-pgo -j 2`,
      `python scripts/perf/pgo_train.py --build build-pgo`, rebuild with
      `-DPORT_PGO=use`, A/B (~3 h). The profiles go to `build-pgo/pgo-win`.

## 5. Release builds

- [ ] Push `main` with everything; tag `v0.6.0` on the final commit.
- [ ] **Linux:** AppImage in the Ubuntu 22.04 container
      ([APPIMAGE.md](APPIMAGE.md), `--pgo build-pgo11/pgo`), sources tar
      (`collect_sources.py --runtime-commit fedfad51acc2860b2916a1eef32ba4a500ff31ef`),
      a `.sha256` for each file. The type2 runtime and 7-Zip come from the 0.5.2
      AppImage (hashes checked against its `app/package.json`).
- [ ] **Windows:** from the MSYS2 UCRT64 shell, in a checkout of the final
      `main` ([WINDOWS_PACKAGE.md](WINDOWS_PACKAGE.md)):
      `build_windows_package.py --bundle --pgo build-pgo/pgo-win`,
      `collect_windows_sources.py`, the zip
      `BattrideWarRecomp-v0.6.0-windows.zip`, the
      `BattrideWarRecomp-v0.6.0-windows-sources.tar`, a `.sha256` for each.

## 6. Release tests

| Test | Linux (AppImage) | Windows (zip) |
|---|---|---|
| `--check` / the launcher opens without a noticeable delay | [ ] | [ ] |
| New import through play: movie, menus, STG02 | [ ] | [ ] |
| Upgrade from a 0.5.2 data folder rebuilds once and keeps saves | [ ] | n/a |
| GPU list in the launcher; the chosen GPU is used (`using device N`) | [ ] | [ ] |
| Interrupted first data installation recovers | [ ] | [ ] |
| **Stop** during extraction and build leaves no temporary files | [ ] | [ ] |
| Window, Alt+Tab and taskbar show **Battride War** | [ ] | [ ] |
| Closing the window ends the game; controller, keyboard and mouse | [ ] | [ ] |
| `bench_combat.py` on both GPUs against 0.5.2 | [ ] | [ ] |
| 30-minute session without dropouts or stalls | [ ] | [ ] |

**Criterion:** no regression against 0.5.2 on Linux, and Windows working end
to end on both GPUs.

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

1. Update phase A (step 3) in 0.6.0 or 0.6.1?
2. `tsc=reliable` at boot on the test laptop, for measurement only?
3. When is the Windows machine available for steps 1, 4, 5 and 6?
