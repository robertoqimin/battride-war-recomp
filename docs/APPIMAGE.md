# Linux AppImage

The AppImage contains **no game code or assets**. It includes GCC optimization profiles: counts of how often each branch of the translated code ran while the developer played the opening mission, which make the build on your computer faster. They contain no game code, text, graphics or sound. (see "Optimization profiles" below). It carries the port's public sources (exactly the files in `PUBLIC_FILES.txt`), the pinned ps3recomp SDK, 7-Zip, FFmpeg and a compiler toolchain. On first launch it downloads the official RPCS3 build to decrypt your EBOOT, imports your disc, translates your own EBOOT into C and compiles the game for your CPU. Corresponding source for every bundled binary is published next to the AppImage on each release page (`BattrideWarRecomp-v<version>-sources.tar`; see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)). It runs on distributions with glibc 2.35 or newer; see "System requirements" in the [README](../README.md) for CPUs, GPUs, RAM and disk. It has been run end to end on Fedora (Ryzen 5 4600H, GTX 1650); the RPCS3, 7-Zip and FFmpeg helpers were also started inside Ubuntu 22.04. Other distributions and GPUs require testing. It does not fix the remaining combat performance drops.

## Run

Make the AppImage executable and open it:

```sh
chmod +x BattrideWarRecomp-v0.6.0-x86_64.AppImage
./BattrideWarRecomp-v0.6.0-x86_64.AppImage
```

If FUSE is unavailable, run `./BattrideWarRecomp-v0.6.0-x86_64.AppImage --appimage-extract-and-run`.

Select the Japan BLJS10220 ISO and click **Import ISO & Play**. The first launch:

1. checks the disc listing, then downloads the official RPCS3 build pinned in `packaging/linux/rpcs3.json` (83 MB, from the RPCS3 project's GitHub release; SHA-256 verified). If `~/Downloads` already holds that exact file, or you pass `--rpcs3 /path/to/rpcs3-v0.0.43-20370-0b0fa082_linux64.AppImage`, no download happens. It then extracts and verifies the game files (size and SHA-256), decrypts the EBOOT with RPCS3 and extracts the audio codecs from the firmware update on the disc, and deletes RPCS3 again (`import.log`);
2. translates the EBOOT into PPU/SPU sources with the SDK tools (`build.log`);
3. compiles the game with `-march=native` for this CPU, one job per 2 GB of available memory (`build.log`). This took 13 minutes with 4 jobs on a Ryzen 5 4600H; expect 15-60 minutes;
4. checks that a Vulkan GPU can create a window surface, a swapchain and BC textures (`graphics.log`), then starts the game (`game.log`).

The generated sources and object files are deleted after the build; only `runner/game-runner` stays in the data folder. Later launches reuse it until something it was built from changes: the port sources the build reads, the SDK, the bundled toolchain, the optimization profiles, the EBOOT or the CPU. An update that only changes the documentation or the launcher keeps it (from 0.6.1; 0.6.0 and earlier rebuilt on every new version). No separate firmware download, compiler, development package or administrator access is required; only the RPCS3 download needs an internet connection.

Default data: `~/.local/share/battride-war-port/`. Saves are inside this folder; copy the whole folder to transfer an installation (the game is rebuilt if the other computer has a different CPU). The ISO path is remembered in `~/.config/battride-war-port/settings.json`. If moving an imported installation changes the ISO path, select a new empty data folder and import again; existing imports are preserved. Logs are in `logs/` under the data folder. MangoHud is optional and must be installed on the destination PC if selected.

On laptops with two GPUs the game picks the discrete one. If it picks the wrong device, the `device N:` lines in `game.log` list them and `PS3RECOMP_VK_DEVICE=N` forces one.

Headless checks:

```sh
./BattrideWarRecomp-v0.6.0-x86_64.AppImage --check    # helpers and build tools start
./BattrideWarRecomp-v0.6.0-x86_64.AppImage --import-only /path/game.iso --data-dir /new/empty/data   # import and build
```

## Build the AppImage

Everything is built inside the Ubuntu 22.04 image; `build_appimage.py` refuses any other glibc. Inputs: the original extracted disc under `game/` and the firmware codecs under `firmware/` (only their names, sizes and SHA-256 are recorded, to verify the player's disc), a git checkout of ps3recomp next to this one, the official standalone 7-Zip Linux distribution (`7zz` and `License.txt`) and the official AppImage type2-runtime x86_64 binary. No files are downloaded automatically. Obtain the runtime from https://github.com/AppImage/type2-runtime and verify its published SHA-256.

```sh
podman build -t battride-build:ubuntu22.04 packaging/linux/build-image
G=$(cd .. && pwd)   # parent holding this checkout and ../ps3recomp
podman run --rm --userns=keep-id --security-opt label=disable \
  -v "$G:$G" -v /path/inputs:/path/inputs:ro -w "$PWD" battride-build:ubuntu22.04 \
  python3 scripts/release/build_appimage.py \
    --runtime /path/inputs/runtime-x86_64 \
    --sevenzip-dir /path/inputs/7zip-linux --stage build-appimage/v0.6.0/AppDir \
    --pgo build-pgo11/pgo --output dist/BattrideWarRecomp-v0.6.0-x86_64.AppImage
# Corresponding source for the release page (runs as container root; needs network):
podman run --rm --security-opt label=disable -v "$G:$G" -w "$PWD" battride-build:ubuntu22.04 \
  python3 scripts/release/collect_sources.py --stage build-appimage/v0.6.0/AppDir \
    --out dist/BattrideWarRecomp-v0.6.0-sources.tar --runtime-commit <type2-runtime commit>
```

`--pgo` takes GCC profiles trained with the AppImage's own compiler (see "Optimization profiles"); without it the AppImage builds the game without them. Profiles trained before the sources moved into `port/` (November 2026) carry the old object names: `python3 scripts/perf/move_profiles.py build-pgo11/pgo` renames them once.

Publish the sources tar with every AppImage, from the same build: `collect_sources.py` maps each file listed in `app/system-files.txt` to its Ubuntu package, downloads the source packages of exactly the installed versions, and adds 7-Zip and the AppImage runtime with the libfuse and squashfuse sources it links.

What goes in:

- `usr/share/battride/port/`: the files listed in `PUBLIC_FILES.txt`, nothing else.
- `usr/share/battride/ps3recomp/`: `git archive` of the commit pinned in `recompile.json` (local changes never ship) and a `.ps3recomp-commit` stamp, which `scripts/build/generate.py` accepts in place of a git checkout.
- `usr/toolchain/`: assembled by `scripts/release/appimage_toolchain.py` from the build image's packages: GCC 11, Clang 14, binutils, CMake 3.22, Ninja, patch, the glibc 2.35 and Linux headers, SDL2, Vulkan headers and the static glslang/SPIRV-Tools libraries. The compilers run with `--sysroot`, so the game links against glibc 2.35 whatever the host has; `toolchain.cmake` points CMake at the sysroot, and the package CMake files that hardcode `/usr` are rewritten to follow `CMAKE_SYSROOT`. Tool processes take only glibc and the C++ runtime from the host (`runtime/` holds their other libraries, LLVM 14 included under its own soname).
- `packaging/linux/glslang-compat`: Ubuntu 22.04's glslang installs neither `glslangConfig.cmake` nor `Public/ResourceLimits.h`; the shim supplies both on top of the packaged static libraries, so the SDK stays unmodified. The local build must report `Vulkan: guest programs available (glslang 11.8.0)`, or the launcher stops.

### Optimization profiles

`usr/share/battride/pgo/` holds GCC profile-guided optimization data: for each compiled function of the translated code, how often each branch was taken while the opening mission was played. The player's build passes `-DPORT_PGO=use`, and GCC lays out and inlines the hot paths accordingly. The files contain counters and checksums of the functions' control flow, no code, text, graphics or sound. `profile.json` records the GCC version, SDK commit and EBOOT they match; the importer uses them only when the SDK and EBOOT match, and `build_appimage.py` refuses profiles made with a different GCC than the bundled one. Objects are named relative to the build directory (`-fprofile-prefix-path`), so the profiles apply in the importer's fresh build directory; the two SDK sources compiled into the game keep the SDK's absolute path in their profile names, and the importer renames those for its own SDK copy.

GCC 11 also hashes each function's source file name into the checksums and ids the profiles are matched by, so PGO builds run the compiler through `scripts/build/pgo_relpaths.py`, which passes this tree's paths relative to the build directory. The names then read the same on the player's computer, provided training used the importer's layout: the sources in `<work>/port`, the build in `<work>/build`. Until November 2026 training ran in the repository, and the game the AppImage built got only part of its profiles (in 0.6.0 testing: 57.9 against 58.9 fps, worst 5 s 50.4 against 53.7, for the same profiles).

To produce them, build an AppImage stage first, then `python3 scripts/perf/pgo_tree.py --stage build-appimage/<v>/AppDir --root <new folder>` (copies the stage's port tree to `<new folder>/port` and translates the EBOOT there with the stage's SDK). Configure `<new folder>/build` with the importer's options (`packaging/importer.py`, `build_runner`), the stage's `usr/toolchain/toolchain.cmake` with its `sysroot/usr/bin` first in `PATH`, `-DPS3RECOMP_DIR=<stage>/usr/share/battride/ps3recomp` and `-DPORT_PGO=generate -DPORT_PGO_DIR=<new folder>/pgo`, build it, then run `python3 scripts/perf/pgo_train.py --build <new folder>/build`: two sessions boot the game, pass the menus and the tutorial with scripted input and fight for a minute each, ending with SIGTERM so the training build writes its profiles. The profiles only fit the C++ sources they were trained on; retrain after changing any C++ file of the port, the generated code layout, or the SDK.

Measured on a Ryzen 5 4600H with the bundled GCC 11, four alternating 60-second STG02 combat runs each: 47.9 against 46.4 fps on average, p95 frame time 24.5 against 28.5 ms, frames over 33 ms 3 against 17, worst five seconds 42.9 against 35.4 fps. The same profiles compiled for `znver3`, `skylake` and `x86-64-v2` targets without a single function losing its profile.

The runner is linked with `PORT_PORTABLE=ON`, which hides the static shader-tool symbols (an exported SPIRV-Tools copy can break RADV), and with an `$ORIGIN/lib` RPATH: the launcher copies SDL2 and the libraries it needs from the AppImage (`game_libs` in `app/package.json`) into `runner/lib/`.

Nothing in the AppImage uses `LD_LIBRARY_PATH`. It would leak the bundled libraries into host programs started from it: ninja runs every command through `/bin/sh`, which on Arch-based systems is bash linked against the host's newer readline, and the bundled Ubuntu readline broke it. Instead, `patchelf` gives each bundled executable a relative DT_RPATH (inherited by the libraries it loads), the toolchain's tools point at `usr/toolchain/runtime/`, and libraries with their own RUNPATH (LLVM, libpulse) are rewritten the same way.

The builder refuses existing stage/output paths. It never bundles libc, the dynamic loader, the C++ runtime, the GL/Vulkan loaders, DRM/GBM/LLVM, or the core X11/xcb/Wayland libraries that Mesa and NVIDIA link; those come from the host so they match its GPU driver. Toolkit helpers that are not on every host (GTK 4 and libadwaita, the librsvg icon loader, SDL2, xcb-util, xkbcommon) are bundled. RPCS3 runs through its own `AppRun`, which uses RPCS3's `checkrt` C++ runtime on hosts older than it needs. Unresolved imports seen at build time are listed in `app/unresolved-build-libraries.txt` (none are expected). Dependency paths are recorded in `app/library-sources.json`; component hashes are in `app/package.json`. Build-image package documentation (including the toolchain's licenses) is retained, and `app/CREDITS.md` is shown by the launcher's **Credits & licenses** button.

## What changed in 0.6.0

- **Faster fights:** in the STG02 benchmark the game the AppImage builds ran 58.5 fps against 54.8 in 0.5.2 on an integrated Radeon (RENOIR), worst 5 seconds 53.9 against 46.3, and 59.7 against 57.0 on a GTX 1650 (worst 5 s 57.9 against 49.0). Guest stores take one compare, and the optimization profiles now apply in full to the player's build: they used to be trained in the repository and only partly matched the AppImage's paths ([PERFORMANCE.md](PERFORMANCE.md)).
- **Minimized window:** minimizing the game and restoring it shows the game again; while minimized it stops presenting frames.
- **Graphics card:** Settings, Rendering, **Graphics card** lists the GPUs by name; Automatic still picks the dedicated one. The launcher runs `graphics-probe` once when it opens to list them.
- **Interrupted first start:** if the game is closed while it installs its data on first start, the launcher removes the half-installed data before the next start and the game installs it again, instead of stopping at the title screen.
- **Window name:** the game window is titled **Battride War** (it was `ps3recomp`), and its Wayland app_id and X11 class are `battride-recomp`, as the AppImage's desktop file.
- **Windows:** the same release has a Windows package ([WINDOWS_PACKAGE.md](WINDOWS_PACKAGE.md)). The sources moved into `port/`, the scripts into `scripts/{build,perf,release}/` and the packaging into `packaging/{linux,windows}/`; the optimization profiles were renamed to match.
- **Launcher:** the Graphics card list no longer offers software renderers (llvmpipe). Ctrl+C or closing the terminal during `--import-only` now also stops 7-Zip, RPCS3 or the build and removes the half-written folder, and Stop removes the unpacked RPCS3 in `tools/` as well.
- **Pipeline cache:** the game keeps its compiled graphics pipelines in `~/.cache/battride-war-port/`, one file per GPU, so scenes after the first run do not wait for the driver to compile them again (on the GTX 1650, 113 ms of compiling down to 13 ms for the first fight; up to 1.5 s when the driver's own cache is empty). Deleting the folder is safe.
- The game is rebuilt once on first launch.

## Chapter 4 audio in 0.5.2

In the first mission of Chronicle chapter 4 (stage `stg08`) the sound broke up from start to end: about 72% of the audio blocks came too late. The audio task loads a small SPU module from the game's executable there and runs it on every block; it was interpreted. The module is now translated like the rest of the task, and `patches/ps3recomp/` makes four SPU byte operations use SSE4.1 on x86 instead of byte loops (the AppImage applies it to its SDK copy). The mission now loses about 0.2% of the blocks in combat, like the other stages ([ISSUES.md](ISSUES.md) item 19). The game is rebuilt once on first launch.

## Window on recent Mesa in 0.5.1

On CachyOS with an RX 6750 XT (RADV, Wayland) the game ran without a window: `game.log` showed `vkGetPhysicalDeviceSurfaceFormatsKHR failed (VkResult 5)` and `window disabled, continuing headless`. The surface lists more than 64 format/colour-space pairs, the backend asked for 64 and took the resulting `VK_INCOMPLETE` for an error. It now reads up to 256 and accepts `VK_INCOMPLETE`. The game is rebuilt once on first launch.

## What changed in 0.5.0

- **Launcher** rebuilt on GTK 4 and libadwaita 1.1 (the Ubuntu 22.04 floor), with Game, Settings, Keyboard, Controller and Log pages; the Log page follows the import, build and game logs live. A GameMode switch appears when `gamemoderun` is installed.
- **Display:** windowed, borderless and fullscreen modes at a chosen size, AMD FSR 1 upscaling, 16:9 kept with black bars, and internal rendering at 2x, 3x or 4x.
- **Controls:** keyboard and mouse as pad 1 with remapping and mouse camera sensitivity; controllers get a radial stick dead zone, button remapping, the first one connected as player 1, a bundled SDL_GameControllerDB and connection messages in the log.
- **Window:** Wayland when the session has it, and IMMEDIATE presentation when MAILBOX is missing, so the window no longer waits for vsync through XWayland.
- **Performance:** average fps in STG02 fighting went from about 48 to about 58 on the test laptop (Ryzen 5 4600H, GTX 1650): busy threads on cores of their own, render targets no longer recreated every frame, faster vertex and index processing on the RSX walker, and the guarded fast store for every translated store ([PERFORMANCE.md](PERFORMANCE.md)).
- **Shadows** sit at the characters' feet again: the shadow map was sampled from an attachment wider than the map.
- The game is rebuilt once on first launch.

## No translated code in the sources in 0.4.1

The integration call for the graphics command arenas used to be applied to the generated PPU code as a patch, which carried a few context lines of translated code. `scripts/build/generate.py` now inserts it by the guest return address alone; the generated file is identical, so the compiled game and the optimization profiles are unchanged.

## Performance in 0.4.0

The game is compiled with the optimization profiles above, and the runner has three measured improvements (details in [PERFORMANCE.md](PERFORMANCE.md)): guest lwmutex locks no longer read the clock twice per call, which cut frames over 33 ms by about two thirds; the translated code is backed by transparent huge pages where the kernel allows; and the profiles shorten the busiest moments of combat further. Builds from 0.3.x are rebuilt once on first launch.

## Arch-based distributions in 0.3.2

On CachyOS the build stopped at CMake's first compiler check with `/bin/sh: symbol lookup error: undefined symbol: rl_trim_arg_from_keyseq`: the build environment's `LD_LIBRARY_PATH` made the host's bash load Ubuntu's older readline. Version 0.3.2 drops `LD_LIBRARY_PATH` everywhere in favour of relative RPATHs (see above). The game build was verified end to end in an Arch Linux container (bash 5.3, readline 8.3, glibc 2.44).

## RPCS3 download and published sources in 0.3.1

Version 0.3.0 carried RPCS3's AppImage, with its Qt and other libraries, without their sources. Version 0.3.1 no longer redistributes RPCS3: the launcher downloads the official pinned build when importing and deletes it afterwards. Every other bundled binary's source is published as `BattrideWarPort-v0.3.1-sources.tar` beside the AppImage, and the launcher has a **Credits & licenses** button ([CREDITS.md](../CREDITS.md)).

## Builds on the player's computer in 0.3.0

Version 0.2.0 carried a prebuilt runner, that is the game's code translated from the EBOOT, so it could not be published. Version 0.3.0 ships no runner: the launcher generates the sources from the player's EBOOT and compiles them with the bundled toolchain. The CPU requirement drops to SSE4.1, and every CPU gets a native build.

## Portable build in 0.2.0

Version 0.1.1 bundled Fedora's glibc and loader and needed the build machine's own libraries. Version 0.2.0 is built in Ubuntu 22.04 against the host graphics stack: an x86-64-v3 runner with shader translation linked statically, a launcher CPU check for the x86-64-v3 features, the Vulkan window check before the game starts, and RPCS3's `checkrt` runtime on hosts with an older libstdc++.

## ISO import fix in 0.1.1

The initial 0.1 package inadvertently depended on Fedora’s `/usr/libexec/7zip/7z.so`, so a different PC could fail at the first manifest file (`PS3_GAME/ICON0.PNG`). Version 0.1.1 uses the official standalone `7zz`, checks actual UDF support in `--check`, and records the ISO reader output in the import log. Disc size/hash validation remains enabled.
