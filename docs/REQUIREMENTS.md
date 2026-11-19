# System requirements

The short version is in the [README](../README.md#requirements). This page has the full checks.

## Linux (AppImage)

The AppImage contains no game code or assets. It includes GCC optimization profiles: counts of how often each branch of the translated code ran while the developer played the opening mission, which make the build on your computer faster. They contain no game code, text, graphics or sound. On first launch it imports your disc, translates your EBOOT and compiles the game on your computer with its bundled toolchain (see [APPIMAGE.md](APPIMAGE.md)). Only one system has run the game end to end: Fedora on a laptop with a Ryzen 5 4600H and GTX 1650 (Radeon integrated graphics alongside; the backend picks the discrete GPU). The on-device build was also verified in an Arch Linux container, and the import helpers in Ubuntu 22.04. Everything below meets the checks in `packaging/launcher.py`, `packaging/graphics_probe.c` and the Vulkan backend, but has not been run.

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

## Windows (zip package)

<!-- TODO: move here the Windows details from WINDOWS_PACKAGE.md once they are settled (Windows 10 testing, installer in 0.6.1). -->
See [WINDOWS_PACKAGE.md](WINDOWS_PACKAGE.md). The CPU and GPU requirements are the same as on Linux.
