# Windows package

The Windows package is the AppImage's counterpart: a folder, published as a zip,
with **no game code or assets**. It carries the launcher, the port's public
sources (exactly the files in `PUBLIC_FILES.txt`), the pinned ps3recomp SDK, GCC
optimization profiles (counts of how often each branch of the translated code
ran while the developer played the opening mission; no game code, text, graphics
or sound), 7-Zip, and a subset of MSYS2 UCRT64: the compilers, CMake, Ninja,
Python, GTK 4, SDL2 and FFmpeg. On first launch it downloads the official RPCS3
build to decrypt your EBOOT, imports your disc, translates your own EBOOT into C
and compiles the game for your CPU. Every bundled binary is listed with its
version in `usr/toolchain/packages.json`; their licences and corresponding
source are described in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

It has been run end to end on Windows 11 (Ryzen 5 4600H, GTX 1650 and its
integrated Radeon). Windows 10, other GPUs and other computers need testing.

## Run

1. Extract the zip to a **short folder**, for example `C:\Games`, so the package
   is `C:\Games\BattrideWarRecomp`. Windows stops at 260 characters in a path
   and the package nests files 124 deep, so its folder may have at most 130
   characters; the launcher says so before building if it is longer. Do not run
   it from inside the zip.
2. Open `BattrideWarRecomp.cmd`. It starts the launcher with the bundled Python
   and GTK, without a console window.
3. Select the Japan BLJS10220 ISO and click **Install & Play**. The first launch
   downloads RPCS3 (35 MB, from the RPCS3 project's GitHub release, SHA-256
   verified), extracts and verifies the disc, decrypts the EBOOT and extracts the
   audio codecs, deletes RPCS3, translates the EBOOT, and compiles the game for
   this CPU. With the profiles, compiling took 21 minutes with 3 jobs on a
   Ryzen 5 4600H; expect 15-60 minutes. Then the game opens.
4. On its first start the game installs its data (a few minutes, with an
   installation screen). **Let it finish.** If the window is closed during it,
   the launcher removes the half-installed data before the next start and the
   game installs it again.

**Stop** ends whatever the launcher is running (download, extraction, build or
game) and leaves no temporary files; the next **Install & Play** continues.

Default data: `%LOCALAPPDATA%\battride-war-port`; settings in
`%APPDATA%\battride-war-port\settings.json`. The data folder may have at most
100 characters (the build nests about 155 below it). Saves are inside the data
folder, and logs in its `logs` folder (`import.log`, `build.log`, `graphics.log`,
`game.log`).

**Graphics card:** on laptops with two GPUs the game uses the dedicated one.
Settings, Rendering, Graphics card picks another by name; the integrated Radeon
of a Ryzen 5 4600H ran the opening mission's fights at about 46 fps, against 56
on the GTX 1650.

## Windows security prompts

The package and the game it builds are not signed.

- **SmartScreen** may warn about the zip's contents when they come from the
  internet: "More info", then "Run anyway". Or, before extracting, open the
  zip's Properties and tick **Unblock**.
- **Smart App Control** (Windows 11) once blocked the freshly built
  `game-runner.exe` ("An Application Control policy has blocked this file")
  and allowed it on the next try. If it keeps blocking it, Smart App Control
  has to be turned off in Windows Security, App & browser control. On some
  Windows versions it cannot be turned back on without reinstalling Windows,
  so decide for yourself.
- **Microsoft Defender** scans every file the build writes, which slows it
  down. Nothing needs to be changed for it to work.

## Build the package

From the MSYS2 UCRT64 shell, in a checkout of the `windows` branch with the
packages from `docs/WINDOWS.md` plus `mingw-w64-ucrt-x86_64-{python-capstone,
python-pycryptodome,python-gobject,gtk4,libadwaita,adwaita-icon-theme,ffmpeg}`,
the original extracted disc under `game/` and the firmware codecs under
`firmware/` (only their names, sizes and SHA-256 are recorded):

```bash
python scripts/build_windows_package.py --stage ../BattrideWarRecomp \
    --sevenzip-dir "C:/Program Files/7-Zip" --bundle --pgo build-pgo/pgo-win
python scripts/collect_windows_sources.py --package ../BattrideWarRecomp --out ../BattrideWarRecomp-sources.tar
"/c/Program Files/7-Zip/7z.exe" a -tzip -mx=5 BattrideWarRecomp-windows.zip ../BattrideWarRecomp
```

`--pgo` takes profiles from `scripts/pgo_train.py` (see `docs/WINDOWS.md`);
without it the player's build runs about 25% slower. `--check` instead of
`--out` asks the servers that every source exists without downloading it.
