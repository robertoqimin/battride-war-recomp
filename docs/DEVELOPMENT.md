# Development

## Building from source

Follow [BUILD.md](BUILD.md) for Fedora dependencies, the pinned SDK revision, data layout, the EBOOT hash and PPU/SPU generation.

After preparing the SDK and local data in a fresh checkout:

```bash
../ps3recomp/.venv/bin/python scripts/build/generate.py --jobs 4
cmake --preset linux-vulkan \
  -DPython3_EXECUTABLE="$PWD/../ps3recomp/.venv/bin/python"
cmake --build --preset linux-vulkan -j2
./scripts/run-vulkan.sh
```

Generation refuses to overwrite existing results. Expect about 593 MiB of PPU sources and more for building. Each generated chunk takes about 1.5 GB of RAM to compile, so limit parallel jobs to the memory you have (`-j2` on 16 GB with a desktop running). The build uses clang for C and GCC for C++. The AppImage itself is built in an Ubuntu 22.04 container ([APPIMAGE.md](APPIMAGE.md)), including the optional optimization profiles (`scripts/perf/pgo_train.py`).

## Components

The repository is laid out as:

| Folder | Contents |
|---|---|
| `port/core/` | the runner, HLE adapters, PPU loader and memory fast paths, threads |
| `port/graphics/` | GCM, the RSX draw engine and vertex fetch, the Vulkan backend, FSR 1 |
| `port/audio/` | cellAudio pacing and the SAIL movie player |
| `port/spu/` | the SPU interpreter, channels, coherency and the audio task |
| `port/io/` | file system and input |
| `include/`, `shaders/`, `third_party/` | headers the lifted code includes, the FSR shader, vendored code |
| `patches/ps3recomp/` | the port's changes to the pinned SDK |
| `scripts/build/` | EBOOT translation and source export; `scripts/run-vulkan.sh` runs a local build |
| `scripts/perf/` | benchmark, PGO training, frame reports |
| `scripts/release/` | the AppImage and the Windows package, with their source collectors |
| `packaging/` | the launcher and importer (`linux/`, `windows/`: per-platform pieces) |
| `tests/` | unit tests, registered with CTest (`ctest --test-dir <build> -L unit`) |
| `docs/` | build, performance, issues, packaging; `docs/reports/` holds dated reports |

`src/recomp/`, `spu/` and `analysis/` are generated from your game and never published.

- `stubs.cpp`: game HLE adapters, blocking `sys_lwcond`, the optional graphics buffer expansion, and the CPU sampler (`PORT_PROFILE`).
- `main.cpp`: the runner, including the huge-page remapping of the translated code (`PORT_HUGEPAGES=0` disables it) and the PGO training hooks.
- `input_bridge.c`: controllers (first connected on port 0, radial dead zone, button remapping, `third_party/SDL_GameControllerDB/`) and keyboard and mouse merged into pad port 0 (`vulkan_bridge.c` captures the mouse).
- `thread_affinity.c`: gives the busiest threads physical cores of their own.
- `sync_bridge.c`: the SDK's Win32 compatibility layer with clock-free zero-timeout waits, used by every guest mutex lock.
- `gcm_bridge.c`, `rsx_engine_bridge.c`, `vulkan_bridge.c`, `loader_bridge.cpp`: local adaptations of SDK implementations (the official checkout remains separate), including the RSX depth-comparison direction, texture border colours and shadow-map snapshot size that the shadows need (`PORT_SHADOW_REVERSE=0` and `PORT_DEPTH_SNAPSHOT_FULL=1` restore the earlier behaviour), and two colour targets per guest address so passes that share one are not recreated every frame. `PORT_ENG_PROFILE=1` times the walker's per-draw work.
- `vulkan_bridge.c` also holds the display modes and the internal resolution scale, which it applies at the backend's edges (target sizes, viewport and scissor, the fragment programs' screen position, and pixels crossing to or from guest memory) while the engine keeps the PS3's sizes.
- `fsr1_bridge.c`, `shaders/fsr1.comp`, `fsr1_spv.h`: AMD FSR 1 upscaling for the window, with AMD's headers in `third_party/fsr1/` (`scripts/build/build_fsr1_shaders.py` regenerates the SPIR-V).
- `rsx_vertex_bridge.c`: exact SIMD paths for common vertex formats, attribute-by-attribute batch fetching and a direct-table reference remap (`PORT_VERTEX_VERIFY=1` checks the batch path against the per-vertex one).
- `include/port_ppu_memory.h`: guarded inline memory access in selected hot PPU chunks; `store_wrap.c` gives every other translated store the same fast path through the linker. Reserved SPU lines and diagnostics retain the original accessors.
- `spu_bridge.c`: runs the audio task rejected by the static translator, translated where `scripts/build/lift_audio_spu.py` could and interpreted elsewhere (`PORT_AUDIO_INTERPRET=1` interprets all of it); another SPU image is generated and registered statically.
- `spu_interp_bridge.c`, `spu_channels_bridge.c`: the SDK's SPU interpreter with a decode cache, and its channel layer with cached environment lookups.
- `sail_bridge.cpp`, `sail_decoder.hpp`, `sail_audio.hpp`: PAMF callbacks, video, and audio. See [SAIL.md](SAIL.md).
- `audio_bridge.c`, `fs_bridge.cpp`: local extensions to SDK audio and VFS.
- `scripts/build/generate.py`: translates your EBOOT and inserts the one integration call `stubs.cpp` needs, located by its guest return address.
- `packaging/`: the AppImage launcher and importer.
- `scripts/perf/bench_combat.py`: the combat benchmark (scripted route into STG02, average, p95, worst 5 s, per-thread CPU); `scripts/perf/pgo_train.py` trains the optimization profiles on the same route.

Local firmware codecs and their limitations are documented in [FIRMWARE.md](FIRMWARE.md). Diagnostic switches and measurement tools are in [PERFORMANCE.md](PERFORMANCE.md).
