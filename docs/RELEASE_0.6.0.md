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
- [ ] Push `main`: the commits since `1302673` stay local until the release
      is published (the README already announces 0.6.0).
- [x] Leftovers removed (`../bw-test-0.6.0`, the partial sources folder). The
      AppImage built on November 15 is superseded by any change below and is
      rebuilt in step 5.

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

- [ ] `bench_combat.py --runs 3` on each GPU.
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
| 2 | **Guarded fast memory path for the top caller functions** instead of whole chunks (whole-chunk widening was rejected twice for code size): `func_005AAC60`, `func_003C7A98`, then the next ones while they pay. Needs a per-function switch in the generated code. | up to 4.4% | `scripts/build/generate.py`, `include/port_ppu_memory.h` |
| 3 | **The pending inline change in `include/port_ppu_memory.h`**, done together with the PGO retrain of step 4 (it invalidates the profiles of the fast chunks). | part of the 16-17% | `include/port_ppu_memory.h` |
| 4 | **Pipeline cache on disk** (`VkPipelineCache`; all four `vkCreate*Pipelines` calls pass `VK_NULL_HANDLE` today). No pipeline is created during the STG02 fight, so no fps gain there: it shortens scene loads after the first run and is the candidate cause of the 945 ms hitch at 1,027 draws ([WINDOWS.md](WINDOWS.md)). | loads; hitch | `port/graphics/vulkan_bridge.c` |

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
