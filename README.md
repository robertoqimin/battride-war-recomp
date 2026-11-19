<div align="center">

# Battride War Recomp

**Kamen Rider: Battride War (PS3, Japan) recompiled into a native Linux and Windows program, from your own disc.**

[![Latest release](https://img.shields.io/github/v/release/robertoqimin/battride-war-recomp?label=latest)](https://github.com/robertoqimin/battride-war-recomp/releases/latest)
![Linux](https://img.shields.io/badge/Linux-AppImage-FCC624?logo=linux&logoColor=black)
![Windows](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D6?logo=windows&logoColor=white)
![Vulkan](https://img.shields.io/badge/renderer-Vulkan-AC162C?logo=vulkan&logoColor=white)
[![License: MIT](https://img.shields.io/badge/license-MIT-1baf7a)](LICENSE)

<!-- TODO: once README.pt-BR.md exists: [Português](README.pt-BR.md) -->

<!-- TODO: screenshots taken by you: the launcher, a fight at 1x and the same fight at 4x with FSR 1.
![The launcher](docs/images/launcher.png)
![STG02, 4x internal resolution](docs/images/stg02-4x.png)
-->

</div>

> [!IMPORTANT]
> - **You need your own disc:** the Japan release, **BLJS10220**, as an ISO. No game files, firmware, keys or
>   compiled game are included or downloaded from this project.
> - **The game is in Japanese** (text and voices). This port does not translate it.
> - **Experimental:** playable through the opening mission (STG02). The rest of the game has not been played yet.
> - Not affiliated with the game's rights holders.

## Contents

- [Quick start](#quick-start)
- [Requirements](#requirements)
- [Status](#status)
- [Controls](#controls)
- [Display options](#display-options)
- [Performance](#performance)
- [Updating, uninstalling and saves](#updating-uninstalling-and-saves)
- [Troubleshooting](#troubleshooting)
- [Reporting a problem](#reporting-a-problem)
- [For developers](#for-developers)
- [Credits and license](#credits-and-license)

## Quick start

Download from the [latest release](https://github.com/robertoqimin/battride-war-recomp/releases/latest).

| Linux | Windows |
|---|---|
| 1. Download the `.AppImage`. | 1. Download the `-windows.zip`. |
| 2. `chmod +x BattrideWarRecomp-*.AppImage` and run it. | 2. Extract it to a **short folder** such as `C:\Games`, not inside `Downloads` and not from inside the zip ([why](#path-too-long-windows)). |
| 3. Select your ISO and click **Install & Play**. | 3. Open `BattrideWarRecomp.cmd`, select your ISO and click **Install & Play**. |

**The first launch takes a while (15–60 minutes)**, and only happens once. The launcher:

1. downloads the official RPCS3 build (SHA-256 checked) to read your disc, and deletes it afterwards;
2. checks the disc files and decrypts your EBOOT;
3. translates your EBOOT into C;
4. compiles the game for your CPU.

On its first start the game then installs its data, with an installation screen. **Let it finish.**

> **Getting the ISO:** dump your own disc with a PS3 or a compatible Blu-ray drive. RPCS3's
> [quickstart guide](https://rpcs3.net/quickstart) explains how. Other regions and digital copies are not supported.

## Requirements

| | Linux | Windows |
|---|---|---|
| **System** | glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and newer), FUSE 3 | Windows 11 (tested), Windows 10 (not tested yet) |
| **CPU** | x86-64 with SSE4.1 (Intel Penryn and newer, every AMD Ryzen, AMD FX) | same |
| **GPU** | Vulkan 1.0 with BC (DXT) textures: NVIDIA GeForce 600 and newer, AMD Radeon HD 7000 and newer, Intel HD 4000 and newer | same |
| **RAM** | about 8 GB for the build | same |
| **Disk** | about 8 GB in the data folder, plus about 4 GB while building | same, plus the 742 MB package <!-- TODO: confirm the Windows numbers --> |
| **First launch** | internet for the RPCS3 download (83 MB) | internet for the RPCS3 download (35 MB) |

Per-core CPU speed sets the frame rate in crowded fights. Full lists of CPUs, GPUs, drivers and distributions:
[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md).

## Status

**Playable through the opening mission.** The intro movie, title screen, menus, the opening cutscene and mission
**STG02** work. STG02 was completed by hand, returning to mission select.

What works in the tested parts:

- **Graphics:** scenery, characters, enemies, HUD, special-attack effects and character shadows, compared with
  footage of the original game.
- **Audio:** music, voices and effects.
- **Saves and data:** data installation and saves.
- **Movies:** PAMF playback.
- **Input:** controllers (SDL2), keyboard and mouse, together.
- **Display:** windowed, borderless or exclusive fullscreen, internal resolution up to 4×, AMD FSR 1.

**Known issues** (full list in [docs/ISSUES.md](docs/ISSUES.md), in Portuguese):

- The most crowded fights can drop to about 45 fps. Ordinary fighting runs at 58–60.
- Audio can break up during heavy fights, at the same moments as the frame drops.
- One large untextured polygon was seen for a single frame at the start of a special attack and could not be
  reproduced.

**Not validated yet.** Help testing is welcome:

- [ ] the rest of the game after STG02
- [ ] sessions longer than about 30 minutes
- [ ] a clean exit on every path
- [ ] the ATRAC audio codec
- [ ] Windows 10, other GPUs and other computers

## Controls

Any controller SDL2 recognises (DualShock, DualSense, Xbox, Switch Pro and the community
[SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)) works as the PS3 pad. Keyboard and mouse work
at the same time.

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

- Click in the window to let the mouse move the camera (in fullscreen it is captured automatically). Alt, or
  switching to another window, releases it.
- **Remapping:** the launcher's **Keyboard** page changes every key and the mouse sensitivity. The **Controller**
  page sets the stick dead zone and which button each PS3 button reads.
- Controller buttons follow their position, not their label: the bottom face button is × on every layout.
- Rumble works.

The files and variables behind these settings: [docs/CONFIG.md](docs/CONFIG.md).

## Display options

On the launcher's **Settings** page. **Alt+Enter** or **F11** switches between the window and fullscreen while
playing.

- **Display mode:** windowed (resizable), borderless fullscreen, or exclusive fullscreen at a chosen resolution.
- **Rendering (internal resolution):** the PS3 renders at 1280 × 720. The port can render 2×, 3× or 4× larger in each
  direction, shadows included, so the detail is drawn, not stretched.
  <!-- TODO: measure 1x/2x/3x/4x again on 0.6.0; the old numbers were from v0.4.x. -->
- **Upscaling:** AMD FidelityFX Super Resolution 1.0 or bilinear when the picture is shown larger than it was
  rendered.
- The picture keeps 16:9, with black bars on other screen shapes.

## Performance

Measured on a Ryzen 5 4600H with a GTX 1650: 60 seconds of fighting in STG02, with the game built on this computer
by each package.

| Build | Average | p95 frame | Worst 5 seconds |
|---|---|---|---|
| AppImage v0.6.0 | 59.7 fps | 19.1 ms | 57.9 fps |
| Windows package v0.6.0 | 57.7 fps | 21.3 ms | 52.0 fps |

The game caps itself at 60 fps. In the heaviest moments its own main thread is the limit, so per-core CPU speed
matters more than the GPU.

- For a few more frames: plug in laptops and set the CPU to performance mode (`powerprofilesctl set performance` on
  Linux).
- How the port got from about 6 fps to 60, and how to measure it: [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

## Updating, uninstalling and saves

| | Linux | Windows |
|---|---|---|
| **Data** (game files, saves, logs) | `~/.local/share/battride-war-port` | `%LOCALAPPDATA%\battride-war-port` |
| **Settings** | `~/.config/battride-war-port/settings.json` | `%APPDATA%\battride-war-port\settings.json` |
| **Graphics pipeline cache** | `~/.cache/battride-war-port` | `%LOCALAPPDATA%\battride-war-port` |

The data folder can be changed on the launcher's **Game** page. Its old name (from when the project was called
Battride War Port) is kept, so installs and saves from older versions carry over.

- **Updating:** download the new release and run it with the same data folder. Saves and settings stay.
  **A new version currently compiles the game again (15–60 minutes)**, even when only a little changed.
  <!-- TODO: update this line once the incremental build (plan, part 10) ships. -->
- **Uninstalling:** delete the AppImage or the package folder, then the folders above. **The data folder holds your
  saves:** copy them first if you want to keep them. <!-- TODO: name the saves subfolder. -->
- **The pipeline cache** (`pipelines-*.bin`) can always be deleted safely, and is rebuilt as you play. On Windows
  it sits in the data folder itself: delete only those files, not the folder.

## Troubleshooting

### "Windows protected your PC"

The package and the game it builds are not signed yet.

- **SmartScreen:** click "More info", then "Run anyway". Or, before extracting, open the zip's Properties and tick
  **Unblock**.
- **Smart App Control** (Windows 11) once blocked the freshly built `game-runner.exe`. Trying again worked. If it
  keeps blocking it, see [docs/WINDOWS_PACKAGE.md](docs/WINDOWS_PACKAGE.md#windows-security-prompts) before turning
  it off: on some Windows versions it cannot be turned back on without reinstalling Windows.

### Path too long (Windows)

Windows stops at 260 characters in a path, and the compilers nest files deep inside the package. So:

- the package folder can have at most **130 characters**;
- the data folder at most **100**.

The launcher says so before building. Move the folder somewhere short, such as `C:\Games\BattrideWarRecomp`.
Don't run the launcher from inside the zip.

### The game does not start, or the window stays black

- **The first start shows the data installation.** Let it finish. If the window was closed during it, the launcher
  removes the half-installed data and starts over the next time.
- **Graphics check failed:** look at `graphics.log` in the `logs` folder. The GPU needs Vulkan with BC textures, and a
  current driver.
- **Laptops with two GPUs:** the game picks the dedicated one. Settings → Rendering → Graphics card picks another.

### My controller is not recognised

`game.log` names every controller as it connects, or names a device SDL does not know together with its GUID. A
mapping for it can be given in `SDL_GAMECONTROLLERCONFIG` ([docs/CONFIG.md](docs/CONFIG.md)).

## Reporting a problem

Open an [issue](https://github.com/robertoqimin/battride-war-recomp/issues) with:

- your system (distribution or Windows version), CPU, GPU and driver;
- what you did and what happened;
- the `logs` folder from the data folder (`import.log`, `build.log`, `graphics.log`, `game.log`). The launcher's
  **Log** page has a button that opens it.

## For developers

- **Building from source:** [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) (overview and repository layout),
  [docs/BUILD.md](docs/BUILD.md) (Linux) and [docs/WINDOWS.md](docs/WINDOWS.md) (Windows, MSYS2 UCRT64).
- **Packages:** [docs/APPIMAGE.md](docs/APPIMAGE.md) and [docs/WINDOWS_PACKAGE.md](docs/WINDOWS_PACKAGE.md).
- **Performance and diagnostics:** [docs/PERFORMANCE.md](docs/PERFORMANCE.md).
- **Configuration outside the launcher:** [docs/CONFIG.md](docs/CONFIG.md).
- **Firmware codecs:** [docs/FIRMWARE.md](docs/FIRMWARE.md). **Movies:** [docs/SAIL.md](docs/SAIL.md).
- **Planned work:** [docs/ROADMAP.md](docs/ROADMAP.md). **Publishing:** [docs/PUBLISHING.md](docs/PUBLISHING.md).

The repository contains only the port's code, configuration, tests and instructions. `src/recomp/`, `spu/` and
`analysis/` are generated from your game and are never published.

## Credits and license

The people and projects behind this port: [CREDITS.md](CREDITS.md).

MIT licensed, keeping the PS3Recomp notice, in [LICENSE](LICENSE). Adapted files and dependencies with other licenses
are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Releases carry only the tools and the port's public sources (the Linux AppImage and the Windows package), plus the
corresponding source of every third-party binary in them. The game is built from the player's own disc, and no
compiled game is distributed.
