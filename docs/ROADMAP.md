# Roadmap

Planned work that is understood but not started. Each entry says what is wrong
today, what to change and how to check it.

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
