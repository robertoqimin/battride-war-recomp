# Battride War Recomp

An **experimental** native Linux port of Kamen Rider: Battride War (Japan), **BLJS10220**, built with PS3Recomp 0.17.0 and Vulkan. This project is not affiliated with the game's rights holders.

This repository contains the port's code, configuration, tests and instructions. **It does not include the game, ISO, EBOOT, firmware, keys, saves, a compiled game executable, or generated PPU/SPU sources.** You need your own copy of the Japan disc.

**Download:** the [latest release](https://github.com/robertoqimin/battride-war-recomp/releases/latest) (v0.5.2) is an AppImage that imports your ISO, translates your own EBOOT and compiles the game on your computer. See [System requirements](#system-requirements-appimage).

**Windows:** a native Windows build is in progress on the `windows` branch. The opening mission plays through there, built from source with MSYS2; there is no Windows release yet. See [docs/WINDOWS.md](docs/WINDOWS.md).

The project was called **Battride War Port** (repository `battride-war-port`) until v0.5.0. Old links redirect here, and the data folder keeps its old name, `~/.local/share/battride-war-port`, so existing installs and saves carry over.

## Current status (October 26, 2026)

**Playable through the opening mission.** The intro movie, title screen, menus, the opening cutscene and mission **STG02** work, and STG02 was completed by hand, returning to mission select. The rest of the game has not been played.

What works in the tested parts:

- **Graphics:** scenery, characters with animation, enemies, HUD, special-attack effects and character shadows, which fall from the characters' feet. Shadows, lighting and effects in the opening cutscene and STG02 were compared with footage of the original game and match.
- **Audio:** music, voices and effects, with the game's audio SPU task, the module it loads and its MP3 codec translated to native code.
- **Saves and data:** game data installation and saves; 21,107 installed files matched the disc byte for byte.
- **Movies:** PAMF playback through `cellSail`.
- **Input:** controllers through SDL2, and keyboard and mouse (see [Controls](#controls)); both can be used together.
- **Display:** windowed, borderless fullscreen or exclusive fullscreen at a chosen resolution, internal rendering at up to 4× the PS3's resolution, AMD FSR 1 upscaling, and 16:9 kept with black bars (see [Display options](#display-options)).
- **Launcher:** a GTK 4 / libadwaita window that imports the disc, builds the game, and sets the display, keyboard, controller and performance options, with a live log (see [Using the AppImage](#using-the-appimage)).

Known issues ([docs/ISSUES.md](docs/ISSUES.md) has the full list, in Portuguese):

- **Frame drops in the most crowded fights.** Ordinary fighting runs at about 55-58 fps, but the heaviest moments drop to about 45 fps: there the game's own command building on its main thread is the limit (see [Performance](#performance)).
- **Audio** was measured without gaps in the title music and scripted combat, but a manual session still reported breakups during heavy fights, coinciding with frame drops.
- **One graphics glitch:** a large untextured polygon was seen for a single frame at the start of a special attack and could not be reproduced.
- **Not validated:** the full game, sessions longer than about 30 minutes, a clean exit on every path, and the ATRAC audio codec.

### Performance

Measured on a Ryzen 5 4600H with a GTX 1650 with `scripts/bench_combat.py`: 60-second windows of STG02 fighting (the first fight, after the tutorial prompt), with the game built on this computer by the AppImage:

| Build | Average | p95 frame | Worst 5 seconds |
|---|---|---|---|
| AppImage v0.5.0 (bundled GCC 11, optimization profiles) | 55.6 fps (runs of 57.0, 51.9 and 57.9) | 21.2 ms | 46.1 fps |
| v0.4.x code on the same route | about 48 fps | 24 ms | about 45 fps |

These are guest frames per second: the game runs its logic in real time and caps itself at 60. The run at 51.9 fps met a heavier fight (up to 960 draws a frame against about 800). In the heaviest moments the game's own command building on its main thread is the limit ([docs/PERFORMANCE.md](docs/PERFORMANCE.md)); a manual session before v0.4.0 reported about 25 fps on this laptop in the heaviest fights, not re-measured since.

What got it there, from about 6 fps at the start (details in [docs/PERFORMANCE.md](docs/PERFORMANCE.md)):

- `-O2` for the translated code, batched Vulkan command buffers, non-blocking presentation (MAILBOX when available) and a frame clock driven by the game's own fences.
- Real blocking for the game's lightweight condition variables (`sys_lwcond`): the SDK's polling version made the render thread redraw each frame dozens of times and overflow its 4 MiB command buffers.
- The audio SPU task and its codec translated instead of interpreted.
- Inline guest memory access in the hottest translated chunks, SIMD vertex decoding, and reuse of Vulkan constant buffers, descriptors and pipeline binds.
- Guest mutex locks no longer read the system clock twice per call, which cut frames over 33 ms by about two thirds.
- The translated code runs from transparent huge pages where the kernel allows them.
- GCC profile-guided optimization, trained on the opening mission and shipped with the AppImage.
- In 0.5.0: the busiest threads on physical cores of their own, render targets kept instead of recreated every frame, faster vertex and index processing on the RSX walker, and the guarded fast store for every translated store.

## System requirements (AppImage)

The AppImage contains no game code or assets. It includes GCC optimization profiles: counts of how often each branch of the translated code ran while the developer played the opening mission, which make the build on your computer faster. They contain no game code, text, graphics or sound. On first launch it imports your disc, translates your EBOOT and compiles the game on your computer with its bundled toolchain (see [docs/APPIMAGE.md](docs/APPIMAGE.md)). Only one system has run the game end to end: Fedora on a laptop with a Ryzen 5 4600H and GTX 1650 (Radeon integrated graphics alongside; the backend picks the discrete GPU). The on-device build was also verified in an Arch Linux container, and the import helpers in Ubuntu 22.04. Everything below meets the checks in `packaging/launcher.py`, `packaging/graphics_probe.c` and the Vulkan backend, but has not been run.

**First launch:** an internet connection to download the official RPCS3 build (83 MB) that reads your disc, unless you already have that exact file; about 8 GB of RAM (the build runs one compiler job per 2 GB available, at least one), about 8 GB of free space in the data folder (game files, plus about 4 GB while building) and time: the compile took 12-14 minutes with 3-4 jobs on the 4600H; expect 15-60 minutes depending on the CPU. It happens once; the built game is reused until the AppImage version or the CPU changes.

**CPU:** x86_64 with SSSE3 and SSE4.1. The game is compiled with `-march=native`, so each CPU gets code for its own instruction set (AVX2 where available).

| Supported | Not supported |
|---|---|
| Intel Core 2 45 nm (Penryn/Wolfdale/Yorkfield) and newer | Intel Core 2 65 nm (Conroe/Merom): no SSE4.1 |
| Every Intel Core i3/i5/i7/i9 and Core Ultra generation, Xeon from Nehalem | Intel Atom Bonnell/Saltwell (Silvermont and newer are fine) |
| Intel Pentium/Celeron from Silvermont/Goldmont, N100/N200 | AMD Athlon 64, Phenom, Phenom II (K8/K10): SSE4a only |
| AMD FX (Bulldozer/Piledriver), A-series APUs, Jaguar | ARM and RISC-V |
| Every AMD Ryzen, Threadripper and EPYC generation | |

Per-core speed decides the frame rate in combat: each frame is the RSX command walker followed by the game's main thread, one after the other. CPUs slower per core than a Ryzen 5 4600H will drop further in crowded fights; old CPUs without AVX2 will be noticeably slower. With four or more physical cores the busiest threads get a core each (`PORT_THREAD_PIN=0` turns that off).

**GPU:** Vulkan 1.0 with a graphics queue that can present to the window, `VK_KHR_swapchain` and `textureCompressionBC` (PS3 DXT textures). The launcher's graphics check rejects devices without BC textures.

| Vendor | Supported | Driver |
|---|---|---|
| NVIDIA | GeForce 600 (Kepler) and newer: GTX 700/900/10/16, RTX 20/30/40/50 | Proprietary (470 legacy for Kepler); NVK (Mesa 24.1+) for Turing and newer |
| AMD | Radeon HD 7000 (GCN 1) and newer: R7/R9, RX 400/500, Vega, RX 5000-9000, Ryzen APUs, Steam Deck | Mesa RADV (`amdgpu` kernel driver for GCN 1/2) |
| Intel | Ivy Bridge/Haswell graphics and newer (HD 4000+, UHD, Iris Xe), Arc A/B | Mesa ANV (`hasvk` for Gen7/Gen8) |

Not supported: GPUs without Vulkan (NVIDIA Fermi and older, AMD TeraScale / HD 6000 and older, Intel Sandy Bridge and older). Software Vulkan (lavapipe) passes the check but is far too slow. The GTX 1650 is not the limit at the frame rates above; older integrated and entry-level GPUs may be.

**Distribution:** glibc 2.35 or newer, with the host Vulkan loader (`libvulkan.so.1`) and GPU driver installed. The AppImage does not bundle libc, the Vulkan/GL loaders, Mesa or the X11/Wayland libraries. FUSE 3 (`fusermount3`) is needed, or run with `--appimage-extract-and-run`.

| Supported | Not supported |
|---|---|
| Ubuntu 22.04 / 24.04 and newer; Kubuntu, Xubuntu, Linux Mint 21+, Pop!_OS 22.04+, Zorin OS 17+, elementary OS 7+ | Ubuntu 20.04 and older, Linux Mint 20 |
| Debian 12 (Bookworm), 13 (Trixie) | Debian 11 and older |
| Fedora 36+; Nobara, Bazzite | RHEL / AlmaLinux / Rocky 9 (glibc 2.34) and older |
| RHEL / AlmaLinux / Rocky 10 | openSUSE Leap 15.x |
| openSUSE Tumbleweed, Leap 16 | musl distributions (Alpine, Void musl) |
| Arch, Manjaro, EndeavourOS, CachyOS, Garuda | NixOS without `steam-run` / `appimage-run` |
| Recent SteamOS 3 (Steam Deck): likely, unverified | |

On hosts whose libstdc++ is older than RPCS3 needs (below `GLIBCXX_3.4.32`, e.g. Ubuntu 22.04 and Debian 12) RPCS3's own `AppRun` uses its bundled `checkrt` libstdc++/libgcc_s. No compiler or development package is needed on the host: the bundled Ubuntu 22.04 toolchain builds against its own sysroot.

**For more frames:** set the CPU governor to performance (`powerprofilesctl set performance`), leave the launcher's **GameMode** switch on if GameMode is installed, and keep laptops plugged in.

## Using the AppImage

```sh
chmod +x BattrideWarRecomp-v0.5.2-x86_64.AppImage
./BattrideWarRecomp-v0.5.2-x86_64.AppImage
```

The launcher has five pages:

- **Game:** your ISO and the data folder (game files, saves, logs). **Install & Play** in the header imports the disc and builds the game the first time, and starts it afterwards; it becomes **Stop** while something runs.
- **Settings:** display mode, resolution, upscaling, internal resolution, MangoHud and GameMode.
- **Keyboard** and **Controller:** bindings, mouse sensitivity, stick dead zone and controller buttons (see [Controls](#controls)).
- **Log:** the import, the build and the game's output as they happen, with copy and save buttons and the logs folder.

Settings and bindings are saved as you change them and apply the next time the game starts. The status and the frame rate stay in a bar at the bottom of every page. For a report, send the `logs/` folder from the data folder (default `~/.local/share/battride-war-port`).

## Controls

A controller (anything SDL2 recognises: DualShock, DualSense, Xbox, Switch Pro...) works as the PS3 pad. Keyboard and mouse work at the same time, as pad 1:

| Keyboard / mouse | PS3 pad |
|---|---|
| W A S D | left stick (move) |
| mouse | right stick (camera) |
| left button, J | □ (normal attack) |
| right button, I | △ |
| middle button, L | ○ |
| Space, K | × (jump) |
| Q / E | L1 / R1 |
| Left Shift / R | L2 / R2 |
| V / C | L3 / R3 |
| arrow keys | d-pad |
| Enter / Backspace | ○ / × (confirm / back in menus, as on a Japanese PS3) |
| Esc | START (pause) |
| Tab | SELECT |

Click in the window to let the mouse move the camera (in fullscreen it is captured automatically); Alt or switching away releases it. Close the game with the window's close button.

**Remapping:** the launcher's **Keyboard** page lists every pad control: the pencil replaces its key (press the key or mouse button to use), **+** adds another, the clear button unbinds it; changes are saved at once. The same page sets the mouse camera sensitivity (0.1 to 5.0, default 1.0), inverts the camera's up/down, and can bind keys to the camera for playing without a mouse. Keys follow the keyboard layout (on AZERTY, the key labelled Z is Z). The launcher writes the choice to `controls.cfg` in the data folder; outside the launcher, point `PORT_CONTROLS` at such a file (one `control = key, key` per line, SDL key names such as `W`, `Space`, `Left Shift`, or `Mouse Left`/`Middle`/`Right`/`X1`/`X2`). `PORT_MOUSE_SENSITIVITY` overrides the file's sensitivity.

**Controller:** the first controller connected is player 1, and when it is unplugged the next one takes its place. Buttons follow their position, not their label: the bottom face button is × on Xbox, PlayStation and Nintendo layouts alike. The launcher's **Controller** page sets the stick dead zone (default 0.15 of the stick's travel; raise it if the camera or character drifts with the sticks released) and which controller button each PS3 button reads (`pad_cross = A` and so on in `controls.cfg`; names `A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `L3`, `R3`, `Start`, `Back`, `Guide`, `Up`, `Down`, `Left`, `Right`, `Misc`, `Touchpad`, `Paddle1` to `Paddle4`). Rumble works. Besides the mappings inside SDL, the AppImage loads the community [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) (`PORT_CONTROLLER_DB`), so controllers newer than its SDL 2.0.20 are recognised. `game.log` names each controller as it connects (`[cellPad] controller on port 1: ...`), or names a device SDL does not know together with its GUID; a mapping for it can be given in `SDL_GAMECONTROLLERCONFIG`, which takes precedence over the database.

## Display options

The launcher's **Settings** page sets how the game is shown; Alt+Enter or F11 switches between the window and fullscreen while playing.

- **Windowed:** a resizable window, starting at the chosen size.
- **Borderless fullscreen:** a window covering the desktop at its own resolution.
- **Fullscreen:** exclusive fullscreen, switching the monitor to the chosen resolution (the closest it offers).
- **Rendering (internal resolution):** the PS3 renders the game at 1280 × 720. The port can render it 2×, 3× or 4× larger in each direction (2560 × 1440, 3840 × 2160, 5120 × 2880): every render target, shadow map included, is that much bigger, so geometry, shadows and effects are drawn at the higher resolution instead of being stretched. Measured in STG02 combat on a GTX 1650 with v0.4.x: about 45 fps at native resolution, 42 at 2×, 31 at 3× and 21 at 4×; above 2× that GPU becomes the limit.
- **Upscaling:** when the rendered frame is shown larger than it was rendered, it is upscaled with **AMD FidelityFX Super Resolution 1.0** (edge-adaptive upscaling and sharpening) or plain bilinear filtering; shown smaller (for example 2× on a 1080p screen), it is filtered down with a single bilinear sample per pixel, which at 4× on a 1080p screen skips part of the rendered detail (a proper area downscale is planned: [docs/ROADMAP.md](docs/ROADMAP.md)). The picture always keeps 16:9, with black bars on other aspect ratios.

Outside the launcher, the same settings are environment variables of the game: `PORT_DISPLAY_MODE` (`windowed`, `borderless`, `fullscreen`), `PORT_DISPLAY_SIZE` (for example `2560x1440`), `PORT_RENDER_SCALE` (`1` to `4`), `PORT_DISPLAY_FILTER` (`fsr`, `linear`), `PORT_DISPLAY_STRETCH=1` to fill the window, and `PORT_FSR_SHARPNESS` (sharpening in stops, default 0.2; 0 is the strongest). `PORT_WINDOW_DUMP=<file>` writes one frame as the window receives it, for checking without a screenshot tool (after `PORT_WINDOW_DUMP_FRAME` frames, or when the file named by `PORT_WINDOW_DUMP_TRIGGER` appears). `PORT_FPS_LIMIT=<fps>` caps the frame rate.

## Building from source

Follow [docs/BUILD.md](docs/BUILD.md) for Fedora dependencies, the pinned SDK revision, data layout, the EBOOT hash and PPU/SPU generation.

After preparing the SDK and local data in a fresh checkout:

```bash
../ps3recomp/.venv/bin/python scripts/generate.py --jobs 4
cmake --preset linux-vulkan \
  -DPython3_EXECUTABLE="$PWD/../ps3recomp/.venv/bin/python"
cmake --build --preset linux-vulkan -j2
./run-vulkan.sh
```

Generation refuses to overwrite existing results. Expect about 593 MiB of PPU sources and more for building. Each generated chunk takes about 1.5 GB of RAM to compile, so limit parallel jobs to the memory you have (`-j2` on 16 GB with a desktop running). The build uses clang for C and GCC for C++. The AppImage itself is built in an Ubuntu 22.04 container ([docs/APPIMAGE.md](docs/APPIMAGE.md)), including the optional optimization profiles (`scripts/pgo_train.py`).

## Components

- `stubs.cpp`: game HLE adapters, blocking `sys_lwcond`, the optional graphics buffer expansion, and the CPU sampler (`PORT_PROFILE`).
- `main.cpp`: the runner, including the huge-page remapping of the translated code (`PORT_HUGEPAGES=0` disables it) and the PGO training hooks.
- `input_bridge.c`: controllers (first connected on port 0, radial dead zone, button remapping, `third_party/SDL_GameControllerDB/`) and keyboard and mouse merged into pad port 0 (`vulkan_bridge.c` captures the mouse).
- `thread_affinity.c`: gives the busiest threads physical cores of their own.
- `sync_bridge.c`: the SDK's Win32 compatibility layer with clock-free zero-timeout waits, used by every guest mutex lock.
- `gcm_bridge.c`, `rsx_engine_bridge.c`, `vulkan_bridge.c`, `loader_bridge.cpp`: local adaptations of SDK implementations (the official checkout remains separate), including the RSX depth-comparison direction, texture border colours and shadow-map snapshot size that the shadows need (`PORT_SHADOW_REVERSE=0` and `PORT_DEPTH_SNAPSHOT_FULL=1` restore the earlier behaviour), and two colour targets per guest address so passes that share one are not recreated every frame. `PORT_ENG_PROFILE=1` times the walker's per-draw work.
- `vulkan_bridge.c` also holds the display modes and the internal resolution scale, which it applies at the backend's edges (target sizes, viewport and scissor, the fragment programs' screen position, and pixels crossing to or from guest memory) while the engine keeps the PS3's sizes.
- `fsr1_bridge.c`, `shaders/fsr1.comp`, `fsr1_spv.h`: AMD FSR 1 upscaling for the window, with AMD's headers in `third_party/fsr1/` (`scripts/build_fsr1_shaders.py` regenerates the SPIR-V).
- `rsx_vertex_bridge.c`: exact SIMD paths for common vertex formats, attribute-by-attribute batch fetching and a direct-table reference remap (`PORT_VERTEX_VERIFY=1` checks the batch path against the per-vertex one).
- `include/port_ppu_memory.h`: guarded inline memory access in selected hot PPU chunks; `store_wrap.c` gives every other translated store the same fast path through the linker. Reserved SPU lines and diagnostics retain the original accessors.
- `spu_bridge.c`: runs the audio task rejected by the static translator, translated where `scripts/lift_audio_spu.py` could and interpreted elsewhere (`PORT_AUDIO_INTERPRET=1` interprets all of it); another SPU image is generated and registered statically.
- `spu_interp_bridge.c`, `spu_channels_bridge.c`: the SDK's SPU interpreter with a decode cache, and its channel layer with cached environment lookups.
- `sail_bridge.cpp`, `sail_decoder.hpp`, `sail_audio.hpp`: PAMF callbacks, video, and audio. See [README_SAIL.md](README_SAIL.md).
- `audio_bridge.c`, `fs_bridge.cpp`: local extensions to SDK audio and VFS.
- `scripts/generate.py`: translates your EBOOT and inserts the one integration call `stubs.cpp` needs, located by its guest return address.
- `packaging/`: the AppImage launcher and importer.
- `scripts/bench_combat.py`: the combat benchmark (scripted route into STG02, average, p95, worst 5 s, per-thread CPU); `scripts/pgo_train.py` trains the optimization profiles on the same route.

Local firmware codecs and their limitations are documented in [README_FIRMWARE.md](README_FIRMWARE.md). Diagnostic switches and measurement tools are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Publishing and license

The public file allowlist is `PUBLIC_FILES.txt`. To audit the contents and prepare a source ZIP:

```bash
python3 scripts/export_source.py
```

See [docs/PUBLISHING.md](docs/PUBLISHING.md) to package or publish with Git. Preparation does not upload anything automatically.

Credits for the people and projects behind this port are in [CREDITS.md](CREDITS.md).

MIT licensed, preserving the PS3Recomp notice in [LICENSE](LICENSE). Adapted file origins and dependencies with other licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Releases carry only the AppImage of tools and public sources, which builds the game from the player's own disc, and the corresponding source of every third-party binary in it; no compiled game is distributed.
