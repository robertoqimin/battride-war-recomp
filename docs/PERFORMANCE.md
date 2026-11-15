# Performance measurement and tuning

Use the same SDK, CPU, save copy, mission, camera, input sequence, window size,
and capture settings for comparisons. Run one game process at a time. Compare
several alternating runs: the number and behavior of enemies are not deterministic.
Do not compare menus or video playback with combat.

## Frame intervals and CPU samples

```bash
PORT_FRAME_TIMES=/tmp/combat.csv PORT_PROFILE=/tmp/combat.samples ./scripts/run-vulkan.sh
python3 scripts/perf/frame_report.py /tmp/combat.csv --start 180 --end 240
```

The CSV records monotonic **guest frame completion intervals**, after the frame
is handed to Vulkan. It does not measure scanout, GPU execution time, or input
latency. It is buffered and flushed every 60 frames; an abrupt stop can lose the
last partial buffer. Select only the combat window, excluding loads and menus.
The report gives average FPS, median/p95/p99 and maximum interval, and the count
of frames over the 30 FPS budget. Captures and CPU sampling add overhead; use
identical settings on both sides, then check a run without them.

`PORT_PROFILE_HZ` controls the existing CPU sampler (default 1000). Each line is
`<thread id> <address> <caller>`; the last field is the first of the top 32
stack words that points into the executable's code (0 if none), usually the
return address into the caller, also for samples inside libc or the vDSO. It
attributes memory-helper and clock-read samples to the code that made them. Symbolize against `nm -n` output from the exact executable tested.
The process CPU-time timer delivers at most about one sample per millisecond
for the whole process, so samples are shares, not per-thread CPU time; read
`/proc/<pid>/task/*/stat` to see which thread is saturated.
Samples in shared libraries need their loaded address mappings; do not attribute
them to the last executable symbol. CPU samples locate candidates; they do not
by themselves prove that an optimization improves frame pacing.

## Huge pages

On Linux the runner copies the 2 MB-aligned part of its code segment (about 92
MiB, mostly translated PPU code) into anonymous transparent huge pages at
startup and moves them over the original mapping, so the busy PPU thread misses
the instruction TLB less often. Addresses and bytes are unchanged; profiles and
symbols still match. It needs THP in `madvise` or `always` mode
(`/sys/kernel/mm/transparent_hugepage/enabled`); check `AnonHugePages` of the
`r-xp` mapping in `/proc/<pid>/smaps`. `PORT_HUGEPAGES=0` disables it.

Measured in STG02 combat on a Ryzen 5 4600H (`bench.py`-style 60-second windows,
alternating runs): code huge pages 50.8 fps against 49.9 without (three and
five runs), within run-to-run noise. Huge pages for the guest memory arena were
tried and rejected: 45.7 fps over six runs, with twice the frames over 33 ms.

## Guest lwmutex waits

Every guest `sys_lwmutex_lock`/`trylock` starts with a zero-timeout
`WaitForSingleObject` on the mutex's semaphore. The SDK's Win32 layer read the
monotonic clock twice per call, though only waitable timers depend on it; in
crowded combat the `[vdso]` clock reads were about 6.5% of the busy PPU thread.
`sync_bridge.c` compiles the SDK's `win32_compat.c` unchanged except for that
function, which now reads the clock only for timers and bounded waits.

Measured in STG02 combat on a Ryzen 5 4600H, four alternating runs each: 46.6
against 46.4 fps average, p95 28.7 against 30.2 ms, frames over 33 ms 17
against 52 (ranges 10-23 and 43-61), worst five seconds 35.0 against 32.3 fps.
The average barely moves; the dips in the busiest moments shrink.

The audio SPU thread (about 45% of a core) is not the limit on that machine:
its interpreted remainder was 1.2% of its samples, so translating the last
interpreted entries was not pursued.

## Vulkan changes

- Identical VS/PS constant blocks retain their batch UBO slice.
- Pipeline binds are skipped when the pipeline is already bound. Command buffer
  resets invalidate this cache; video and fallback draws use the same binder.
- Identical resolved image views, samplers, UBO slices, and layout reuse the last
  immutable descriptor set within the same batch generation. Pool resets prevent
  stale reuse. Texture uploads do not change a view's identity.
- Adjacent draws with the same framebuffer/render pass can share a pass. Generic
  operations and submission close it before copies, uploads, clears, or snapshots.
  `PORT_VK_MERGE_PASSES=0` restores separate passes for comparison.

Texture content detection remains unchanged: no additional skipped hashes or
approximate dirty tracking were introduced. Such changes need evidence that
hashing is expensive and a reliable way to observe all guest writes.

## Profiled CPU paths

The initial 60-second crowded STG02 run measured about 24.4 FPS. Its CPU
samples put roughly 43% of process CPU in the busy PPU thread, 25% in the FIFO
thread, and 23% in the audio SPU thread. These are sampling shares, not frame
budgets. PPU memory helpers and `func_004B5120` were prominent; vertex decoding
was 16.5% of the FIFO thread's samples. Texture hashes together were about 2.7%
of that thread, so their detection policy was preserved.

`PORT_HOT_PPU_O3` selects O3 only for generated PPU chunk 029, which contains
that tree traversal function. Disable it for comparison with
`cmake --preset linux-vulkan -DPORT_HOT_PPU_O3=OFF`, then rebuild. Other PPU
chunks keep the existing preset optimization. No fast-math is enabled.

`rsx_vertex_bridge.c` has exact four-component SSE4.1 paths for float, UNORM8,
and UINT8 attributes; other formats and component counts retain SDK behavior.
`bash tests/test_vertex_decode.sh` compares 5.4 million decodes with the SDK.
`rsx_vertex_fetch_many` picks each attribute's format once and decodes the
common ones (float, half, UNORM8/UINT8 x4, CMP32, SNORM16) inline instead of
calling `rsx_vertex_decode_element` per vertex; the same test checks the batch
path against single decodes. In real fighting it is neutral (59.0 against
59.2 fps, worst five seconds 54.9 against 54.7, p95 19.4 against 19.8 ms): it
speeds the frame clock thread, and `runTaskMain` sets the frame rate.

## Guarded PPU memory paths

The manual crowd profile collected 16,806 samples over 30 seconds. On the busy
PPU thread (96.4% of one logical CPU in a five-second observation), read32,
write64, and write32 helpers represented 9.2%, 8.6%, and 8.0% of that thread's
samples. The tree traversal function accounted for 5.7%; its earlier 26.9%
figure came from a much shorter, different interval and is not representative
of every combat scene. Percentages are self samples, not inclusive call costs.

`include/port_ppu_memory.h` is force-included only in profiled generated chunks
015, 017, 021, 029 and 030. It does not modify generated files or the SDK:

- Existing inline reads are forced inline, keeping their original predicates
  and fallback for diagnostics, low addresses, raw SPU windows, reference-word
  polling and memory bounds.
- Aligned ordinary 1/2/4/8-byte writes use big-endian relaxed atomic stores when
  no store diagnostic or SPU reservation applies. Aligned accesses cannot cross
  a 128-byte reservation line.
- Reserved lines always delegate to the original accessor, retaining its lock,
  notification and wakeup behavior. Unaligned, low/null, raw SPU/MMIO, out-of-range
  and diagnostic accesses also delegate. Dynamic barrier watches and null-sweep
  diagnostics are checked on every fast-path candidate.

Use `PORT_PPU_MEMORY_SLOW=1 ./scripts/run-vulkan.sh` to disable the new inline **stores**
at startup. To restore both the original read and write compilation paths,
configure `-DPORT_PPU_FAST_MEMORY=OFF` and rebuild. The cache variable
`PORT_PPU_FAST_MEMORY_CHUNKS` controls the selected generated chunks. Windows
retains the existing accessors. These changes require a new manual performance
comparison; unit tests do not establish an FPS gain. Inlining moves CPU samples
from helper symbols into their guest callers, so a lower helper percentage
alone is not evidence of reduced CPU time.

Widening the list was measured and rejected (STG02 combat, Ryzen 5 4600H, four
alternating runs each against the five-chunk build): adding the next twelve
chunks by heavy-combat samples (001, 009-011, 013, 016, 018, 020, 024, 026-028)
gave 46.7 against 45.9 fps but twice the frames over 33 ms (110 against 55),
with a 27 MB larger executable; adding only 013, 020 and 028 gave 46.0 against
45.9 fps and more frames over 33 ms (71 against 48). Combat on that machine is
limited by the busy PPU thread at 98% of a core; the inlined helpers do not
shorten it measurably beyond the original chunks.

```bash
bash tests/test_ppu_memory.sh
CXX=clang++ SANITIZE=1 bash tests/test_ppu_memory.sh
```

The regression checks 300,000 mixed-width memory round trips, byte equivalence,
unaligned and truncated addresses, bounds and diagnostic fallbacks, and the
actual SDK's reservation invalidation/wakeup implementation. ASan/UBSan are
available through Clang on the development system; its GCC ASan runtime is
currently unavailable. The fixture verifies routing to the existing slow
accessors; it does not emulate every MMIO device or every diagnostic.

## Optional GCC PGO

PGO is disabled by default. It applies to GCC C++ (translated PPU and harness),
not the clang C runtime. Use the same build directory and unchanged source/object
paths for training and use:

```bash
cmake --preset linux-vulkan -DPORT_PGO=generate
cmake --build --preset linux-vulkan
# Train on representative gameplay, then exit normally to write GCC .gcda files.
cmake --preset linux-vulkan -DPORT_PGO=use
cmake --build --preset linux-vulkan
# Repeat the same benchmark and compare with the ordinary build.
```

Do not use a forced kill as a training shutdown. GCC writes profiles at a normal
exit, which the runner does not reliably reach, so a training build writes them
on SIGTERM or SIGINT (`kill <pid>` or Ctrl+C) and exits. PGO is experimental and
is not enabled in the validated default build.
Configuration rejects use mode if no .gcda files exist. Investigate missing-profile
or mismatched-profile compiler diagnostics before claiming a trained build.
Measured on a Ryzen 5 4600H with GCC 16, trained on two runs of the STG02
benchmark below (boot, menus and 60 s of combat, each ending with SIGTERM; the
instrumented build runs at about 7 fps): over four alternating 60-second combat
runs against the same source without PGO, 51.3 against 47.2 fps, p95 22.9
against 28.0 ms, frames over 33 ms 4 against 24, worst five seconds 47.0
against 35.2 fps. In a different scripted fight (walking into new groups),
frames with 550-800 draws ran at 50.6 against 36.5 fps. The profiles come from
running the title's translated code: keep them private like the generated
sources, and note that `.gcda` files only match the GCC version that made them.

`PORT_PGO_DIR` chooses the private profile directory (default: build directory/pgo).
Restore the normal build with `-DPORT_PGO=off`. Native CPU builds may not run on
older CPUs; choose `PORT_MARCH` for the intended target when distributing binaries.

## Combat benchmark and the frame's critical path (2026-10-25)

`scripts/perf/bench_combat.py --binary <game> --runs 3` plays the scripted route into STG02 and measures 60 s of fighting: average fps, p95/p99, frames over 16.7/33 ms, 1% low, worst 5 s, per-thread CPU (guest thread names from the log) and the draw-count range of the window. Run-to-run spread of the average is about ±3 fps, so compare builds with interleaved A/B runs (A, B, A, B, ...). Two traps it now avoids: the tutorial prompt pauses the first fight (identical draw counts in every `[fstat]` line), and the scripted menus sometimes stall (the run is retried). `pgo_train.py` has the same fixes and SIGKILLs a stalled attempt so it writes no profile.

The game runs on wall-clock time (opening logos 20.2 s at 60 fps, 20.9 s with `PORT_FPS_LIMIT=20`) and caps itself at 60 fps. A `PS3_FTRACE` timeline shows each frame as the FIFO walker (`frame_clock`) processing the frame's commands (~12.5 ms), then the guest main thread working ~5.6 ms after the frame's fence. In frames over 20 ms the walker stays at ~13 ms and the main thread grows to ~10.5 ms, mostly building the command list (`func_005B2930`, chunk 020). `PORT_ENG_PROFILE=1` times `sink_end`'s parts per frame.

Adopted (Ryzen 5 4600H, GTX 1650; average fighting fps 48 -> 58, worst 5 s 45 -> 47): busy threads on physical cores of their own (`thread_affinity.c`), two colour targets per guest address (the shadow pass and the scene shared address 0 and recreated two images a frame), faster vertex remap/fetch and index reads, a word-wise vertex-program hash, and the guarded fast store for every translated store (`store_wrap.c`).

Measured and rejected: GameMode (no change; amd-pstate already boosts), C runtime at `-O3`, 1 us timer slack, a shorter fence publication gap, inline fast memory in chunks 016/020, profiles retrained on fighting, and GCC 12.3 with its own profiles (58.2 against 58.4 fps).

## The frame clock thread (2026-10-28)

CPU samples of the `frame_clock` thread (FIFO walk, draw engine and Vulkan; about 68% of a core) in STG02 combat, by group: vertex fetch and decode ~22% (`rsx_vertex_decode_element` alone 11%), the FIFO walk and method dispatch ~21%, analysis of the guest fragment program ~12%, texture hashes and lookups ~11%, the NVIDIA driver ~6%. The executable is PIE: subtract its load address (from `/proc/<pid>/maps`) from the sampled addresses before looking them up with `nm`.

`PORT_ENG_PROFILE=1` reads the clock several times per draw. When the kernel has fallen back to the HPET clock source (`/sys/devices/system/clocksource/clocksource0/current_clocksource`; it does so when it finds the TSC unstable at boot), those reads alone dropped combat to about 35 fps; use the sampler there instead.

Fragment program memo (`rsx_engine_bridge.c`): every draw used to walk its fragment program five times (size, structural hash, sampled, gamma and expand units) and clear the 32 KB inline-constant block twice. A program is now analysed once per address and reused while its instruction blocks are unchanged; its inline constants are read again on every draw, since titles patch them in place. `PORT_FP_MEMO=0` turns it off; `PORT_FP_MEMO=check` recomputes every hit with the SDK functions (3.3 million hits, no mismatch, over boot, menus and combat). Three interleaved pairs: 58.3 against 57.0 fps, frames over 16.7 ms 1,696 against 2,782, p95 20.9 against 21.6 ms, worst five seconds 46.4 against 46.1 fps.

A second tutorial prompt pauses the first fight 15-30 s in, and until 2026-10-28 the benchmark only dismissed the first one: 32-49 s of each 60-second window were then a still screen at the game's own 60 fps cap (same draw count in every `[fstat]` line). The averages measured that way, including the ~58 fps above, are mostly that screen. The benchmark now presses START (or CIRCLE) when four `[fstat]` lines in a row show the same draw count, computes its statistics over the moving frames only, and reports the paused seconds (`paused_s`, about 2 s per run); `--keep-logs` also keeps each run's frame CSV. Real fighting, measured that way, runs at about 53-57 fps, and the limit is the guest `runTaskMain` thread at about 91% of a core, not the frame clock (about 55%).

## Native replacement for the guest list lookup

In real fighting `runTaskMain` spends 22.5% of its samples in one 59-line translated function, `func_004B5120`: a lookup, in the list headed at `r3+0x104`, of the node whose key at `+0xC` equals `r4`, over lists hundreds of entries long (about 17,000 calls a second at about 13 us each). The title's own code stores the current entry to `sp-4` and loads it back, and stores 1 to `sp-0xC`, on every step; translated, each step was six guarded guest accesses, three full CR updates and every register in `ctx`.

`ppu_native.cpp` writes it again with the registers in locals, the CR updates folded and the two stack slots stored once on the way out; every other guest load is the same `vm_read32` in the same order, and on return registers, CR and the stack slots hold what the lifted function leaves. `include/port_ppu_native.h`, force-included only into the chunk that defines it (`PORT_PPU_NATIVE_CHUNK`, 029), makes the lifted definition weak, so the linker sends every call to the native one, and keeps it reachable as `port_lifted_func_004B5120`. Configuration turns the replacement off when that chunk does not define the function. `PORT_PPU_NATIVE=0` runs the lifted function; `PORT_PPU_NATIVE=check` runs both and compares (3 million calls, no mismatch, over boot, menus and combat). `-DPORT_PPU_NATIVE=OFF` builds without it.

The function fell from 22.5% to 11.8% of `runTaskMain`. Three interleaved pairs in real fighting (the corrected benchmark, about 2 s paused per run): 57.1 against 55.5 fps (57.0/54.6, 56.5/53.9, 57.9/57.9), p95 21.2 against 22.4 ms, worst five seconds 48.5 against 47.1 fps. Measured with the kernel on the HPET clock source, where the title's timebase reads (`ppu_timebase_now`) cost a further 11-13% of `runTaskMain`.

## VMX: SIMD compares and reciprocal estimates, always inline

After the list lookup, `func_005C79B4` (chunk 021) led `runTaskMain`: straight-line vector maths (reciprocal square-root estimates refined by Newton-Raphson, compares), 3.5% of the thread itself and 2.1% more in VMX helpers it called. GCC left the `static inline` VMX helpers out of line in the large generated functions, a call per vector instruction, and `vcmpeqfp`/`vcmpgefp`/`vcmpgtfp` and `vrefp`/`vrsqrtefp` still worked lane by lane, byte by byte. `include/ppu_vmx.h` now has SSE versions of those five (ordered `cmpps` with the CR6 result; `divps`/`sqrtps`, which round exactly as the scalar `1.0f / f` and `1.0f / sqrtf(f)`, then the same flush and NaN rules) and marks every SIMD float op `always_inline`. `bash tests/test_vmx_simd.sh` compares all nine SIMD ops with the SDK's scalar ones on 8 million vectors in both NJ modes. The executable grows from 88 to 105 MB.

Three interleaved pairs in real fighting: 57.7 against 56.0 fps (57.6/56.8, 57.4/56.5, 58.1/54.8), p95 20.2 against 21.5 ms, frames over 16.7 ms 1,927 against 2,364, worst five seconds 49.6 against 48.7 fps.

Inline guest memory in more chunks, measured again in real fighting (2026-10-28) and rejected again. Callers of the memory helpers were 19.5% of `runTaskMain`, spread over many chunks (020 2.9%, 013 2.4%, 028 1.8%, 010, 024, 018, 016, 027 about 1% each). Adding those eight to `PORT_PPU_FAST_MEMORY_CHUNKS` (no profile mismatch; executable 105 to 129 MB), three interleaved pairs: 54.9 against 57.2 fps, p95 23.2 against 20.2 ms, worst five seconds 44.9 against 50.0 fps, worse in every pair.

## PGO profiles retrained on real fighting (2026-10-28)

The profiles shipped until v0.5.1 came from training sessions that, like the benchmark, stopped dismissing tutorial prompts after the first: most of each session profiled the paused screen. Retrained with the corrected `pgo_train.py` (two sessions, GCC 11.4.0, 93 profiles; the only mismatches are `main()`, which differs in the training build, and the one-time `ppu_hle_register_all()`), on the same code: 59.2 against 58.1 fps (58.8/57.1, 59.5/58.7, 59.3/58.6), worst five seconds 55.1 against 49.9 fps (better in every pair), p95 19.3 against 20.3 ms; `runTaskMain` falls from about 89% to about 85% of a core. The executable shrinks from 105 to 94 MB, as more of the translated code is now known to be cold. The new profiles are in `build-pgo11/pgo`, the previous ones in `build-pgo11/pgo-1025`.

## Clock reads and the store gate (2026-10-28)

The 11–13% of `runTaskMain` in clock reads (`ppu_timebase_now`, the vDSO on this machine's HPET clocksource) is the game's own frame limiter, not work: a sampler that records three callers puts 14.2% of the thread in `func_003C4148` (read the timebase, retry on a zero low word) called from `func_003ABE20`, which sleeps whole milliseconds with `sys_timer_usleep` and then spins on the timebase to the frame deadline. A cheaper clock would only spin more often; it was not pursued.

Guest memory accessors are about 12% of the thread, spread over hundreds of callers (the largest is 1%). Inside `__wrap_vm_write*` the samples spread over the checks before the store. With the diagnostics, null-sweep and bounds checks removed (a measurement-only build), the worst five seconds rose from 54.8 to 56.7 fps in three pairs. `include/port_ppu_store_gate.h` folds those into one bound, `port_ppu_store_end`, which `loader_bridge.cpp` recomputes when diagnostics are set, a null sweep starts or ends, or `ppu_vm_size` is set; `store_wrap.c` now checks alignment, `g_barrier_sync_watch`, that bound and the SPU reservation bitmap. `bash tests/test_ppu_memory.sh` covers the bound and both store paths (inline and `store_wrap.c`). Three interleaved pairs: worst five seconds 56.3 against 54.7 fps (55.8/54.8, 56.4/54.8, 56.6/54.4), average 59.4 against 59.1 (the game's 60 fps cap). The inline stores in the five chunks of `include/port_ppu_memory.h` keep their own checks for now: changing them alters every function in those chunks and needs new PGO profiles.

## The list lookup is latency-bound (2026-10-29, not adopted)

`native_004B5120` is still about 10% of `runTaskMain`. Counted in combat: some 20,000–25,000 lookups a second, nearly all from `func_004C2830` (which, on a rarer path, repeats the lookup with the same key and always gets the same answer), every one a hit, after a walk of about 250 entries on average (80% walk more than 64). Within the function, 37% of the samples wait on the load of the node's key. Two versions with identical results (`PORT_PPU_NATIVE=check`: 4 million calls, 0 mismatches) did not help: guard checks hoisted out of the walk plus a read-only lookahead that prefetches the keys six entries ahead made it 12.5% of the thread and the worst five seconds 55.5 against 56.3 fps (three pairs); the hoisted checks alone measured 9.8% against 10.5% in one profiled run, within noise. Each step waits on the pointer the previous one loaded, so the remaining lever is not walking at all: a key-to-entry cache, which is only correct if every guest function that links, unlinks or re-keys entries invalidates it.

## Where the time goes in a fight, and O3 on the busiest chunks (2026-11-15, not adopted)

STG02 fighting, no PGO, on the integrated Radeon and on the GTX 1650 (details in [RELEASE_0.6.0.md](RELEASE_0.6.0.md), step 1): `runTaskMain` is at 93% of a core with either GPU (48-52 fps), the render thread at 45%, the integrated GPU 31% busy, and the fastest core holds its 4.0 GHz boost, so the CPU thread alone limits the frame rate. Below the 60 fps cap the frame limiter does not spin: clock reads are 0.1% of `runTaskMain` in a fight. Chunks 021 and 015 hold 25% of the thread's samples and were built at `-O2`. Building them at `-O3` too (+0.7 MB) measured, over three interleaved pairs on the integrated GPU: 48.8 against 50.3 fps on average (lower in two pairs of three), worst 5 s 39.8 against 39.9, p95 26.1 against 25.6 ms. Not adopted; only chunk 029 stays at `-O3`.
