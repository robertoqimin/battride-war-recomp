# Bugs, failures and reported problems

Everything reported up to October 25, 2026, with the current state of each item. "Fixed" means corrected and measured in the tests that were run; it does not mean validated across the whole game.

## Summary

| # | Problem | Area | State |
|---|---|---|---|
| 1 | Graphics command buffer overflow (~19 MiB) | Graphics / synchronization | Fixed |
| 2 | Very low and unsteady frame rate in combat | Performance | Fixed in the measured fight (~58 fps average while fighting, worst 5 s ~47 fps) |
| 3 | Frame rate drops while walking and with crowds | Performance | Fixed (MAILBOX; see item 2 for current numbers) |
| 4 | Audio dropouts and crackles | Audio | Partial: still breaks in combat (manual test) |
| 5 | Intermittent excessive brightness | Graphics | Not reproduced (flashes checked against the original) |
| 6 | Wrong colours and special effects | Graphics | Partial: they match the original; one 1-frame artifact seen |
| 17 | Wrong shadows | Graphics | Fixed in the first mission |
| 18 | Shadows in the wrong place during gameplay | Graphics | Fixed in the first mission |
| 7 | Completing missions | Gameplay | Validated in the first mission (manual test) |
| 8 | Full game not validated | Gameplay | Partial: all 10 chapters load and run; completion validated only in STG02 |
| 9 | Long-run stability | Stability | Partial (~70 min and ~25 stage loads without failures) |
| 10 | Heavy frames limited by the game's command assembly | Performance | Open (worst 5 s ~47 fps; target 60) |
| 11 | Audio depends on the local firmware | Audio | Known limitation |
| 12 | ATRAC codec not exercised | Audio | Not used in any tested chapter |
| 19 | Audio breaks in the chapter 4 mission (STG08) | Audio | Fixed (with an SDK patch) |
| 20 | Sound engine warning at 176566 Hz | Audio | Under analysis |
| 13 | Clean shutdown not validated on every path | Stability | Open |
| 14 | Leftover test processes contaminated measurements | Test tools | Worked around |
| 15 | Short `PAD_FILE` presses lost with the faster game | Test tools | Worked around |
| 16 | Character does not move with the d-pad | Test tools | Worked around (`PORT_PAD_STICK_FILE`) |

## Details

### 1. Graphics command buffer overflow — Fixed
- **Symptom:** the opening mission produced ~19 MiB of RSX commands and overflowed the game's two 4 MiB arenas, overwriting renderer data. The earlier workaround allocated an extra 2×32 MiB.
- **Cause:** the SDK's `sys_lwcond` waits by polling every 1 ms and its `signal` does nothing. The game's render thread waits only once for the frame event, so it never blocked and redrew the whole frame dozens of times per flip (708 renders against 9 flips in 15 s).
- **Fix:** a blocking `sys_lwcond` in `stubs.cpp` (commit `3ce00f8`). The mission's frame drops to a peak of ~0.5 MiB. The buffer expansion is off by default (`PORT_GCM_EXPAND_BUFFERS=1` turns it back on).

### 2. Low frame rate in combat — Fixed
- **Symptom:** ~6 fps in combat.
- **Causes and fixes (commits `75611ca` and `05af18b`):**
  - the whole build at `-O0` → `-O2`;
  - one submission with a GPU wait per draw → batched Vulkan commands, with two slots;
  - debug checks on every guest memory write → they return early when none is active;
  - `getenv` in the FIFO loop → read once;
  - a frame clock with a fixed wait and a blocking present → woken by the game, present without waiting for the GPU.
- **Result:** ~30 fps in combat (29.7 / 30.3 / 30.7).
- **Manual test (October 23, 2026):** with many enemies on screen the frame rate still falls to ~20. See item 10.

### 3. Frame rate drop while walking — Fixed
- **Symptom:** 20–24 fps while walking and in scenes with many enemies (reported in the manual test).
- **Cause:** the window's present in FIFO mode (vsync) blocked the FIFO thread until vblank, so a frame of just over 33 ms waited up to 50 ms.
- **Fix:** MAILBOX mode when the driver offers it (commit `22cab93`); `PS3RECOMP_VK_VSYNC=1` keeps FIFO.
- **Result:** 38 fps average and 29.4 at the 10th percentile over 150 s of fighting and walking; ~35 fps average in a ~32 min session.

### 4. Audio dropouts — Fixed in the tested sections
- **Symptom:** dropouts and crackles; the sound came in bursts (4 blocks of sound, 8 of silence).
- **Cause:** the SPU audio task, rejected by the static translator, ran interpreted and filled only ~1/3 of the blocks in time. In combat almost all the work is the `flashMP3.pic` codec, also interpreted.
- **Fix (commit `08c0e61`):**
  - translation of the task below `0x171D0` and of the MP3 codec (`scripts/build/lift_audio_spu.py`);
  - an interpreter with a decode cache;
  - a channel layer without `getenv` per access;
  - SPU code compiled with `-msse4.1`.
- **Result:** zero empty blocks in the title music and in 45 s of combat.
- **Caveats:** long sessions and other stages were not checked.
- **Manual test (October 23, 2026):** the audio still breaks in combat. Since it coincides with the frame rate drops, the suspect is CPU contention: the SPU audio thread and the game's render thread are both near 100%.

### 5. Intermittent excessive brightness — Not reproduced
- **First report:** one posterized yellow and red frame at the mission entrance, while the repeated redraw (item 1) still happened.
- **Check (October 25, 2026):** one frame per second through boot, menus, the opening scene and combat with special moves, measuring average brightness, clipped pixels and colour count. No posterized frame (every combat frame has 180-230 thousand colours). The only white frames are the fade to white and the flash of the opening scene ("俺の名は『ウィザード』"), and a recording of the original game (YouTube `u4YixUZPH30`, from 3:05) has the same sequence: fade to white, about 2 s of black and a new flash that dissolves into the gate scene. The special moves' flashes are the size of the original's.
- **Conclusion:** not reproduced in current versions. The floor darkened by the wrong shadows (item 17) changed the scene's brightness a lot and may have contributed to the report.

### 6. Wrong colours and special effects — Partial
- **Check (October 25, 2026):** compared with the same recording of the original: the moves' effects (red magic circles, fire, the Water style's water, the cannon arm) and the combat lighting match. The spotlight scene only matched after the shadow fixes (item 17).
- **Still seen:** in a single frame, at the start of a special move, a large untextured beige polygon cut diagonally across the bottom of the screen. It did not happen again in other captures; the cause was not found.

### 17. Wrong shadows — Fixed in the first mission
- **Symptom:** characters cast no shadow, and the floor inside the shadow map's reach was all darkened; outside it (a rectangular region with a straight edge) it was bright.
- **Cause:** the game draws the characters' silhouettes into a 1024×1024 Z24S8 shadow map and samples it in the scene (unit t3, DEPTH24_D8 format, `zfunc` LESS, linear filter). On the RSX the texture's depth comparison is `stored depth OP reference`; the SDK's helper did `reference OP stored depth`. With the direction swapped, the result was inverted on every pixel, and the game's shader maths marked the whole floor as shadowed.
- **Fix:** `rsx_engine_bridge.c` reverses the `zfunc` direction (LESS↔GREATER, LEQUAL↔GEQUAL) before translating the shader. `PORT_SHADOW_REVERSE=0` restores the earlier behaviour for comparison.
- **Second cause (the edge):** the shadow map uses the BORDER wrap mode with border colour `0xFFFFFFFF` (depth 1.0: nothing casts a shadow outside the map). The Vulkan backend always used a black border, so everything outside the map read depth 0 and was "in shadow": in the opening scene the spotlight's floor appeared as a bright straight-edged band instead of a round patch. `eng_decode_sampler` now carries the border colour class (transparent black, opaque black or opaque white, Vulkan's fixed colours) through to `vk_eng_sampler`.
- **Verified:** compared with a recording of the original game (YouTube `u4YixUZPH30`, from 3:05): the spotlight scene is practically identical (a round patch of light and the character's long shadow), and in the STG02 fight the floor is lit with each character's shadow starting at their feet. Same performance. Other missions were not checked.

### 7. Completing missions — Validated in the first mission
- **Manual test (October 23, 2026):** the first mission (STG02) was completed by hand, and when it ended the game went to mission select normally. The other missions were not tested.
- **Earlier automated attempt:** in a ~30 min automated session in STG02, 350 enemies were defeated with a top combo of 50, but the objective stayed at "先へ進め" (move on) until the mission's time (30 min) ran out.
- **Conclusion:** the manual test showed the failure was the autopilot's, not the port's.

### 8. Full game — Partial
- **Test (October 29, 2026):** with an RPCS3 save that has the whole story mode completed (every menu mode unlocked), the first mission of each of the Chronicle's 10 chapters was opened and played by the autopilot (`PAD_FILE`/`PORT_PAD_STICK_FILE`, 1.5 to 8 min each), in a single ~70 min session with the window open.
- **Result:**

| Chapter | Stage | Result |
|---|---|---|
| Prologue | STG02 | Loads and runs; spotlight scene and combat normal |
| 英雄の目覚め (1) | STG02 | Loads and runs |
| 英雄の目覚め (2) | STG09 | Loads and runs |
| 英雄の目覚め (3) | STG03 | Loads and runs; timed motorbike mission: the autopilot does not finish in time and the game shows "Mission Failed" and the result screen normally |
| Chronicle 1章 | STG05 | Loads and runs |
| Chronicle 2章 | STG11 | Loads and runs; motorbike riding normal |
| Chronicle 3章 | STG10 | Loads and runs |
| Chronicle 4章 | STG08 | Loads and runs; **audio breaks for the whole mission** (item 19) |
| Chronicle 5章 | STG12 | Loads and runs |
| Final chapter | STG19 | Loads and runs for 8 min: fights against Decade and Skull, form changes, specials; W was defeated and the game went to the result and "try again" normally |

- No hang or port error in the log; 55 fps average over the whole session (59.4 fps in the final mission's 8 min).
- **Missing:** completing the missions (the autopilot does not finish missions; see item 7), the chapters' other ~20 missions, the ending and credits (`MEN_ENDING.msf`, `MEN_STAFFROLL.msf`), Rider Road, Free Mission and the Gallery. That needs playing by hand.

### 9. Long-run stability — Partial
There were ~30 min of continuous mission without a hang and without errors in the log, up to the test script's time limit. Longer sessions, stage changes and repeated loads were not tested.

### 10. Heavy frames limited by the game's command assembly — Open
- **Measured (October 25, 2026, `PS3_FTRACE` timeline while fighting in STG02):** a frame is the FIFO processing by the `frame_clock` thread (~12.5 ms) followed by the `main` thread's work after the frame fence (~5.6 ms). In frames over 20 ms the FIFO stays at ~13 ms and `main` goes to ~10.5 ms; 72% of it is assembling the command list (`func_005B2930`, translated chunk `020`). The statement below, from October 24, that the main thread was saturated at 98% no longer holds: in combat no thread goes above ~80%, and they wait for each other.
- **Done on October 25, 2026** (interleaved A/B, `scripts/perf/bench_combat.py`): a physical core of their own for the busiest threads (`thread_affinity.c`); render targets that were recreated every frame kept in a cache; faster vertex fetch, index reads and pipeline hashing in the FIFO processing; a fast path for every store of the translated code (`store_wrap.c`). Average while fighting: ~48 → ~58 fps; worst 5 s: ~45 → ~47.
- **Tried and rejected on October 25, 2026:** GameMode (no effect on this machine), the runtime's C at `-O3`, a 1 µs timer slack, a shorter interval between fence publications, the inline fast path in chunks 016 and 020, PGO retrained in combat, and GCC 12.3 (58.2 against 58.4 fps).
- **Possible next step:** improving the code ps3recomp's translator generates for the command assembly (keeping PPU registers in local variables, etc.), a change in the SDK.

#### History (October 24, 2026)
- **Measured (October 24, 2026, per-thread CPU profile in heavy STG02 combat):** whenever the frame rate drops below ~50, the game's main PPU thread is at 98% of a core; audio (~45%), FIFO/Vulkan (~50%) and the other PPU thread (~30%) have headroom. The frame rate in the drops depends only on that thread's speed.
- **Done since** (details in `docs/PERFORMANCE.md`): clock reads removed from the game's mutex locks (frames over 33 ms: 52 → 17), translated code in huge pages, and PGO trained on the first mission (worst 5 s: 35 → 43 fps in the AppImage). Tried and rejected: huge pages for the game's memory and the fast memory path in more generated chunks.
- **Still open:** a manual test before v0.4.0 reported ~25 fps in the heaviest fights on the Ryzen 5 4600H (~40 fps on a Ryzen 7 5700X); it was not remeasured with v0.4.x. Remaining options: FMA instructions (`-mfma`, a portability decision) and a newer GCC in the AppImage's toolchain (~+5% measured with GCC 16).

### 11. Audio depends on the local firmware — Known limitation
The MP3 codec's translation is generated only if the local `flashMP3.pic` has the verified SHA-256. Without it the codec runs interpreted and combat will probably have dropouts again. Without any codec the audio task stops (see `docs/FIRMWARE.md`).

### 12. ATRAC codec — Not used in the tested stages
The game did not open `flashATRAC.pic` in the tests, nor in the October 29, 2026 session through the 10 chapters. All the music (`sound/BGM/BGM/*.msf`) is stereo MPEG; if ATRAC is used somewhere, it runs interpreted and may drop out.

### 19. Audio breaks in the chapter 4 mission (STG08) — Fixed (with an SDK patch)
- **Symptom (October 29, 2026):** in the first mission of Chronicle 4章 ("近くて遠い", stage `stg08.gtc`), 60-75% of the audio blocks arrive empty from the start to the end of the mission (`PORT_AUDIO_STATS`: ~680 underruns of 938 per line), starting already in `MIS_ENTER.msf`. In every other tested stage they stay under 0.4%.
- **Not the music:** `MIS_EVENT_BATTLE_BOSS.msf`, which plays in this mission, also played in the final mission without dropouts.
- **Clue:** in the log, each run of the SPU audio task (`[spu_workload] WAIT_SIGNAL ... ran=`) takes 29-39 ms during this mission, against 0-6 ms in the rest of the session.
- **Cause:** `PORT_SPU_PROFILE` showed ~3.7 billion interpreted steps in 0x37000-0x397C0. There the task loads, without relocation and from LS 0x36F80, a 10 KB SPU module embedded in `EBOOT.ELF` (offset 0x686D00; not a firmware codec). This stage runs the module on every audio block.
- **Fix, in two parts:**
  - `scripts/build/lift_audio_spu.py` also translates that module (72 functions, no unsupported instruction), as it already did with `flashMP3.pic`; `spu_bridge.c` accepts several overlays. On its own this lowered the failures to 2-20%: the module's translated code became ~46% of the audio thread's time.
  - In the SDK (`runtime/spu/spu_helpers.h`), `rotqby`, `rotqbyi`, `shlqbyi` and `cwd` had no vector path on x86 (only on NEON) and became byte-by-byte loops. With SSE4.1 versions (`pshufb`), identical to the reference in an exhaustive test with gcc and clang, the module's hottest function went from 11037 to 8244 instructions.
- **Result (PGO build, October 29, 2026):** in the same mission, 0.2% underruns in the first phase's combat and 0 in 3 minutes of the second; the final mission stays clean (8 of 26264). The autopilot did not reach this mission's boss.
- **SDK:** the change is in `patches/ps3recomp/0001-spu-helpers-sse41-byte-rotates.patch`, applied on top of the revision pinned in `recompile.json` by `scripts/release/build_appimage.py` and, in local builds, by hand (`docs/BUILD.md`); CMake stops if it is missing.

### 20. Sound engine warning at 176566 Hz — Under analysis (probably the game's own)
- **Symptom:** `SCREAM: Error! Sound buffer size is too small for this playback frequency (176566 Hz)`, 4 times in a row, in the final mission, near the line `VOICE/SKL/SKL_050.msf` (Skull). It is intermittent: it appeared in 2 of 3 sessions in that mission; in a third, of 8 minutes with the same line playing twice, it did not. It appeared in no other tested stage.
- **Origin (October 29, 2026):** the function `func_000BE628` (the SCREAM library's `CheckBufferSizesWithPitch`, TOC `0x6F3490`) takes a 16-bit integer pitch and computes the frequency as `pitch × 11.71875` (48000/4096: pitch 4096 = 48 kHz). 176566 Hz corresponds to pitch ≈ 15067, that is, 3.68× normal speed (~+22.5 semitones). The warning appears when the voice's buffer is smaller than `frequency × 0.04267 × 2`; the function only warns and does not stop the sound.
- **The pitch comes from integer maths:** the callers (`func_000BBD28` and `func_000C0424`) pass the voice's note and pitch bend adjustments through `func_000BA778` (combines note, bend and modulation) and `func_000BAA08`/`func_000BA86C` (difference between the requested note and the sample's base note → table → multiplication by `0x10F4A`). All of it is integer arithmetic, which translation rarely gets wrong. If there is a port error, it would be earlier, in the bend or modulation values the game passes to the voice.
- **No reference from the original:** RPCS3's TTY logs on this machine are empty, and the save used did not come from a game played here.
- **Next step, if worth it:** capture the note and the bend values when the warning fires. `TTY_BT=playback` already marks the moment, but on Linux the call chain is not printed: `ppu_log_host_chain` in `loader_bridge.cpp` only works on Windows. The simplest way is to compare with the same mission in RPCS3.

### 13. Clean shutdown — Open
Time-limited runs sometimes need SIGKILL (already noted in `docs/BUILD.md`).

### 14. Leftover test processes — Worked around
An old game process survived `pkill -f` and ran alongside the new one, contaminating two measurements. The tests now end the game by PID and confirm no process is left running.

### 15. Short `PAD_FILE` presses lost — Worked around
With the faster game, presses of 8 polls were sometimes not registered (the opening's START, the ○ of the difficulty choice). The scripts now hold the buttons longer.

### 16. Character does not move with the d-pad — Worked around
In this game the character only walks with the left stick, and the SDK only lets the stick be fixed from the start (`PAD_STICK`). `PORT_PAD_STICK_FILE` (commit `1a8c94a`) lets tests drive both sticks from a file.

### 18. Shadows in the wrong place during gameplay — Fixed in the first mission
- **Symptom (manual test, October 25, 2026):** during gameplay, shadows appeared displaced, as loose diagonal silhouettes, away from the characters' feet.
- **Cause:** the game draws the shadow map into a 1024×1024 depth buffer, but the engine creates the buffer at the size that covers the whole screen, 1280×1024 (`eng_zdepth_get`). The copy used as a texture took the whole buffer, and the shader reads it with normalized coordinates: the map was stretched 1.25× horizontally, and each shadow came out displaced, further the more to the right in the map.
- **Fix:** `eng_zdepth_snapshot` copies only the size the game's texture declares (1024×1024, from the top-left corner). `PORT_DEPTH_SNAPSHOT_FULL=1` restores the earlier behaviour for comparison.
- **Verified:** frames of the STG02 fight before and after (recorded with `scripts/perf/bench_combat.py --dump`), compared with the recording of the original game (YouTube `u4YixUZPH30`): with the fix, each character's shadow starts at their feet, short and behind, as in the original; before, it appeared loose on the diagonal. The spotlight scene, which uses the same map, still matches the original (a long shadow from the feet downwards and to the right). Other missions were not checked.
