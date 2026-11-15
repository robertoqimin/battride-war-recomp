# Building from the published sources

This package contains port code and configuration. Game files, firmware, and complete generated PPU/SPU sources remain on the builder's computer. None of this data is downloaded automatically.

## Validated environment

Fedora 44 x86_64, clang 22, GCC 16, CMake 4, Ninja, and NVIDIA GTX 1650. The project uses C17/C++20 and Python 3.11 or later. Other distributions and GPUs still require validation.

The following package names match those used on this Fedora system. Install them in your terminal:

```bash
sudo dnf install git cmake ninja-build clang gcc-c++ python3 python3-pip \
  sdl2-compat-devel vulkan-headers vulkan-loader \
  glslang glslang-devel spirv-tools-devel pkgconf-pkg-config ffmpeg-free
```

A working Vulkan driver for your GPU is required. `ffmpeg` and `ffprobe` must be on PATH for PAMF video playback. FFmpeg development headers and spirv-cross are not required for this port's Vulkan backend.

## 1. Pin the SDK

Keep the official checkout alongside this port's directory:

```text
workspace/
  ps3recomp/
  kamenriderbattridewarsport/
```

Run these commands in the workspace directory to create a fresh SDK checkout:

```bash
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp
git checkout --detach a6d76c895f77eb30b90a806c830544e61821b77f
git apply ../kamenriderbattridewarsport/patches/ps3recomp/*.patch
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
cd ../kamenriderbattridewarsport
```

If the SDK already exists, do not clone over or replace its directory: check the commit and preserve local changes. The patches in `patches/ps3recomp/` are the port's fixes to that commit (the AppImage applies them to its SDK copy too); CMake stops if they are missing. The local `*_bridge.*` files depend on this revision's interfaces. Updating the SDK requires renewed validation.

## 2. Prepare local data

Use a local extraction of your disc and the decrypted ELF matching **BLJS10220**:

```text
kamenriderbattridewarsport/
  game/
    EBOOT.ELF
    PS3_GAME/
      PARAM.SFO
      USRDIR/
        nativePS/
          ...game files...
```

The expected SHA-256 of `game/EBOOT.ELF` is:

```text
a53b7bca5d47490710ba02af5f885e1219d10c7a1a456fcf6e13a48db668a014
```

Check it with `sha256sum game/EBOOT.ELF`. Other executables, updates, or regions have not been validated: the integration call and SPU bridge addresses are specific to this version. This repository does not supply an ISO, EBOOT, keys, firmware, assets, or saves.

Firmware codecs are optional for building but required for the audio paths used by the game. See [FIRMWARE.md](FIRMWARE.md) for the placement of locally obtained files.

## 3. Generate PPU/SPU sources

From the port root, in a fresh checkout:

```bash
../ps3recomp/.venv/bin/python scripts/build/generate.py --jobs 4
```

The script checks the SDK commit and ELF hash, analyzes the executable, generates the 84 PPU files, inserts the graphics buffer integration call (located by the guest return address `0x0045BC64`, so no translated code is kept in the repository), and generates the accepted SPU programs. It refuses to replace existing `src/recomp/`, `spu/`, or `analysis/regeneration/` directories. Use `--verify-only` to check prerequisites only. For a different SDK location, add `--sdk /path/to/ps3recomp`.

PPU generation produces about 593 MiB of sources; also allow space for object files and the executable. On machines with limited RAM, reduce `--jobs` and build parallelism.

SPU generation deliberately uses `--allow-partial`: of the two detected images, only `615f0131…dc0657` is registered statically. The rejected audio image is then translated by `scripts/build/lift_audio_spu.py` below its data tail, with the SPU module it loads from the executable (and its MP3 codec from the firmware, when present), into `spu/battride_audio/`; `spu_bridge.c` runs that code and interprets the rest. The generated registry is checked before installing the results. All generated results are ignored by Git.

A checkout generated before that script existed can add the audio translation in place, then reconfigure:

```bash
../ps3recomp/.venv/bin/python scripts/build/lift_audio_spu.py --sdk ../ps3recomp \
  --elf game/EBOOT.ELF --spu spu --dev-flash firmware/dev_flash
cmake --preset linux-vulkan -DPython3_EXECUTABLE="$PWD/../ps3recomp/.venv/bin/python"
```

The SPU sources, interpreter and channel layer are built with `-msse4.1` (x86-64-v2 CPUs, which current Fedora already requires).

The script uses these tools and options:

```text
ppu_loader.py game/EBOOT.ELF -o <analysis>
ppu_lifter.py game/EBOOT.ELF --functions <analysis>/EBOOT.functions.json
  --hle-stubs <analysis>/EBOOT.imports.json --output <ppu-sources>
  --jobs 4 --chunk-lines 80000
insert port_expand_graphics_buffers() before the call returning to 0x0045BC64
  in ppu_recomp_015.cpp (scripts/build/generate.py)
build_spu_workloads.py --input game/EBOOT.ELF --lifted <spu-sources>
  --out <spu-sources>/spu_workloads.c --constructor --allow-partial
scripts/build/lift_audio_spu.py --sdk <sdk> --elf game/EBOOT.ELF --spu <spu-sources>
  [--dev-flash <dev_flash>]
```

Use the script to run the sequence: it manages temporary directories and checks. `config.toml` is a reference configuration; this generation is controlled by the options above and `recompile.json`.

## 4. Build and launch

```bash
cmake --preset linux-vulkan \
  -DPython3_EXECUTABLE="$PWD/../ps3recomp/.venv/bin/python"
cmake --build --preset linux-vulkan
./scripts/run-vulkan.sh
```

For a different SDK location, add `-DPS3RECOMP_DIR=/path/to/ps3recomp` during configuration. Check that CMake reports `guest programs available` for Vulkan: without glslang, game shaders do not work correctly. The SDK loads Vulkan dynamically.

The game uses the `game/` tree as its VFS and creates installed data/saves in `gamedata/`. The launcher enables the window and shaders. The 2×32 MiB command-buffer expansion is no longer needed now that `sys_lwcond` blocks (see the README) and is off by default; `PORT_GCM_EXPAND_BUFFERS=1 ./scripts/run-vulkan.sh` re-enables it, and `PORT_LWCOND_SDK=1` restores the SDK's polling `sys_lwcond` for comparison. `PORT_GCM_FRAME_STATS=120` prints command volume, draws and buffer usage every 120 flips. `PORT_AUDIO_STATS=1` prints, every 5 s, how often a cellAudio block was still empty when its turn came; `PORT_SPU_PROFILE=<file>` writes where the audio SPU task spends interpreted time; `PORT_AUDIO_INTERPRET=1` runs that task fully interpreted. For scripted tests, `PORT_PAD_STICK_FILE=<file>` reads "<lx> <ly> [<rx> <ry>]" (0-255, 128 centred) from that file on every pad read and reports it as the analog sticks; this title moves the player only with the left stick. There is no automatic time limit; `PORT_TEST_SECONDS=300 ./scripts/run-vulkan.sh` limits a diagnostic run. Graceful shutdown has not been validated on every path; time-limited tests sometimes required SIGKILL.

## 5. Verify

```bash
python3 -m unittest discover -s tests -p test_publication.py
python3 scripts/build/export_source.py
bash tests/test_graphics_buffers.sh
bash tests/test_lwcond.sh
bash tests/test_gcm_jump.sh
bash tests/test_sail.sh
```

The C/C++ tests require generated/build results according to their dependencies. The audio regression uses the local disc's PAMF:

```bash
bash tests/test_sail_audio.sh
```

To check reads at a specified offset:

```bash
c++ -std=c++20 -O0 -ffunction-sections -fdata-sections \
  -Isrc/recomp -I../ps3recomp/include -I../ps3recomp/runtime/ppu \
  tests/test_fs_offset.cpp -Wl,--gc-sections -o build-vulkan/test_fs_offset
./build-vulkan/test_fs_offset
```

No test publishes files to GitHub. The build does not install dependencies or fetch game data automatically.
