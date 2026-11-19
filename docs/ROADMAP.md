# Roadmap

Planned work that is understood but not started. Each entry says what is wrong
today, what to change and how to check it.

## Windows installer (0.6.1)

### Today

The Windows package is a zip (`BattrideWarRecomp-v<version>-windows.zip`,
742 MB in 0.6.0). The player extracts it to a short folder by hand (at most
130 characters, `WINDOWS_PACKAGE_PATH_MAX` in `packaging/importer.py`) and
opens `BattrideWarRecomp.cmd`. Nothing appears in the Start menu or in
"Installed apps", an update means extracting a new folder next to the old
one, and a player who extracts deep under `Downloads` or runs it from inside
the zip only learns about the path limit from the launcher.

### Change

An installer, `BattrideWarRecomp-v<version>-windows-setup.exe`, built with
**Inno Setup** (free, scriptable, LZMA2; the compiler `ISCC.exe` comes from
winget). It carries exactly the stage the zip is made from, so the game code
the player builds does not change.

- **Per user, no administrator:** `PrivilegesRequired=lowest`, default folder
  `{localappdata}\Programs\BattrideWarRecomp`. No UAC prompt, and the
  launcher never writes into the package folder (data stays in
  `%LOCALAPPDATA%\battride-war-port`). Program Files stays possible only if
  the player asks for it.
- **Path limit checked before copying:** the folder page refuses a path over
  130 characters and suggests `C:\Games\BattrideWarRecomp`, instead of the
  launcher saying so after the copy.
- **Shortcuts:** Start menu, desktop optional. They point at
  `BattrideWarRecomp.cmd` with a minimized window, so no console flashes; a
  small native stub can replace the `.cmd` later. The package needs an icon
  (`.ico`); none exists yet.
- **Updates:** a fixed `AppId`, so a newer installer replaces the old one in
  place. `[InstallDelete]` empties the package folder first: the package has
  thousands of files and a stale one from an older MSYS2 could be loaded.
  The data folder and the built game are untouched; the launcher rebuilds
  once when the version changes, as today.
- **Uninstall:** removes the package folder and shortcuts, and asks whether
  to delete the data folder too, **No** by default (it holds saves).
- **Keep the zip** as the portable option beside the installer.
- **Not signed** at first: SmartScreen shows "Windows protected your PC"
  for a new unsigned `.exe` more often than for a zip ("More info", "Run
  anyway"). Signing (for example Azure Trusted Signing, paid monthly) is a
  separate decision.

### Pieces

- `packaging/windows/installer.iss` (template) and
  `scripts/release/build_windows_installer.py --stage <stage> --version <v>`:
  checks the stage, fills in version and paths, runs `ISCC.exe`, writes the
  `.exe` and its `.sha256` (well under GitHub's 2 GiB limit).
- An icon for the launcher, shortcuts and installer.
- Inno Setup's own stub ends up inside the `.exe`: add it to
  `THIRD_PARTY_NOTICES.md` and `CREDITS.md` (Inno Setup licence).
- [WINDOWS_PACKAGE.md](WINDOWS_PACKAGE.md) and the README: install, update and
  uninstall with the `.exe`, the zip as the portable alternative.
- The new files in `PUBLIC_FILES.txt`.

### Cost

About a day for the script, the icon and the docs; building the `.exe` takes
minutes on top of the stage. Tests about half a day on Windows.

### Checks

- **Fresh install** on Windows 11 without administrator rights: no UAC prompt,
  Start menu shortcut opens the launcher without a console window, then
  import, build and play as from the zip.
- **Path limit:** a user name long enough to put the default folder near 130
  characters, and a typed path over the limit (refused with the suggestion).
- **Update:** install over the 0.6.0 zip's data folder (data reused, one
  rebuild, saves kept), and a second installer version over the first (no
  stale files: compare the folder with the stage).
- **Uninstall:** with **No**, the data folder and saves stay; with **Yes**,
  both folders are gone; no shortcut left either way.
- **SmartScreen** on a download from the release page, and Windows 10.

## Area downscaling for internal resolution above the window

### Today

With `PORT_RENDER_SCALE` above 1 the game renders at N × 1280 × 720. When the
window is smaller than that frame, `vk_window_blit` (`vulkan_bridge.c`) shows it
with a single `vkCmdBlitImage` using `VK_FILTER_LINEAR`. FSR 1 only runs when
the window is larger than the frame.

A linear blit takes one bilinear sample per output pixel: the 2 × 2 source
pixels nearest to its centre. It does not cover the source area the pixel
spans.

| Scale → window | Source pixels per output pixel | Blit reads | Result |
|---|---|---|---|
| 2× → 1920 × 1080 | 1.33 × 1.33 ≈ 1.8 | 2 × 2, uneven weights | acceptable, slightly irregular |
| 3× → 1920 × 1080 | 2 × 2 = 4 | those 4 | exact (by coincidence) |
| 4× → 1920 × 1080 | 2.67 × 2.67 ≈ 7.1 | 4 | about 44% of the rendered pixels ignored |
| 4× → 2560 × 1440 | 2 × 2 = 4 | those 4 | exact |
| 4× → 3840 × 2160 | 1.33 × 1.33 | 2 × 2 | acceptable |

In the most common "highest quality" choice, 4× on a 1080p screen, the GPU
renders 14.7 million pixels a frame and almost half never reach the screen.
The samples land at a different phase in each output pixel, so thin edges
(railings, hair, moving silhouettes) keep aliasing and shimmer that the
supersampling should have removed. On a GTX 1650, 4× costs about half the
frame rate (README, "Display options"), so the waste matters.

### Change

A compute pass beside FSR's (`vk_fsr_record`): for each output pixel, average
the whole source rectangle it covers, each source pixel weighted by the part
of it inside that rectangle (partial weights at the fractional edges). No
rendered pixel is dropped and the result is the same at every position. This
area (box) filter is the standard way to resolve supersampling.

- **Linear light:** the frame holds sRGB-encoded values. Averaging them as
  stored darkens high-contrast edges such as light text on a dark background;
  decode to linear, average, encode again.
- **Sharper option:** the box filter is clean and slightly soft. Offer a
  Catmull-Rom or Mitchell kernel scaled to the ratio, or RCAS after the box
  filter. Not Lanczos by default: it rings around the HUD.
- **When:** whenever the frame is larger than the output area in both
  directions. Otherwise the current paths stay: FSR when enlarging, the blit
  for 1:1. When the frame is larger in one direction and smaller in the other
  (`PORT_DISPLAY_STRETCH` on an unusual aspect ratio), keep the blit.
- **Setting:** on by default, no new option needed. Optionally
  `PORT_DISPLAY_DOWNSCALE=box|sharp|linear` (`linear` = today's blit) and a
  matching launcher choice.
- **Fallback:** if the pipeline or its image cannot be created, log once and
  keep the blit, as FSR does.

### Pieces

- `shaders/downscale.comp`, compiled to SPIR-V by the same script as FSR
  (`scripts/build/build_fsr1_shaders.py`) into a generated header.
- Pipeline, descriptor set and an output image at the output size, created
  and resized like `fsr_mid`/`fsr_out`.
- The condition in the present path, and `vk_window_dump` reading the new
  output image.
- The new files in `PUBLIC_FILES.txt`; README "Display options" updated.

The change is limited to presenting the window. Rendering, guest-memory copies
and render-target scaling are untouched.

### Cost

One read of the frame per presented frame: at 4× about 59 MB, well under 1 ms
on a GTX 1650. Rendering at 4× remains the expensive part.

### Checks

- **Image:** dump the same STG02 combat frame at 4× in a 1920 × 1080 window
  (`PORT_WINDOW_DUMP`) with the blit and with the new pass. Compare both with
  a reference made offline from the full 5120 × 2880 frame by exact area
  averaging in linear light; the new pass should match it within rounding.
- **Motion:** watch railings and character edges while moving; the shimmer
  should be gone at 4× → 1080p.
- **Speed:** `scripts/perf/bench_combat.py` at 4× → 1080p, before and after (three
  interleaved pairs); the difference should be within noise.
- **Other cases:** 2× and 3× at 1080p, 4× at 1440p (ratio 2, where the new
  pass must equal the old result), a 4K window, borderless and fullscreen,
  and a GPU where the pass fails to initialise (falls back to the blit).
