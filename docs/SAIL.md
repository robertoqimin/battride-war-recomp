# cellSail playback in this port

The port uses `sail_bridge.cpp` and `sail_decoder.hpp` to play PAMF video
through the system's `ffmpeg` and `ffprobe` executables. ATRAC3plus audio is
demultiplexed from PAMF private packets and decoded by the ATRAC library
already included in the SDK. FFmpeg development packages are not required.
The original SDK remains intact.

Game imports are registered with handlers that receive PPU registers directly.
This preserves guest addresses and 64-bit arguments, including asynchronous
completion tokens. Format and frame structures are written in big-endian order.
Signatures were checked against
[RPCS3's cellSail.h](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.h).

## Behavior

- Boot, open, start, stop, and close operations pass through a queue processed
  by the frame clock. Player and adapter callbacks run through the guest
  dispatcher. The decoding thread never calls game code.
- Descriptors retain the URI and resolve only `/dev_bdvd` files within
  `PS3_VFS_ROOT`. Directory traversal and symlinks outside the root are rejected.
  No path is interpolated into a shell command.
- Probing identifies resolution and frame rate. The decoder forces the MPEG
  demuxer because `.pam` is also an image format extension.
- FFmpeg supplies ARGB/RGBA or YUV422 pixels. At most four frames are queued on
  the host, with three guest buffers obtained through the game's allocation
  callback when provided. Pitch, tag, sessionId, and PTS accompany each frame.
- GetFrame2 respects the video's frame rate and pause state. End of file emits
  SOURCE_EOS only when the decoder finishes successfully and all frames have
  been delivered. After delivering the last frame, the player returns to OPENED.
  This lets the game recognize natural completion without simulating a user Stop.
- Stop cancels the decoding process and joins the thread; Close frees buffers.
  Decoder failures report an error rather than pretending playback completed.
- Audio is delivered as big-endian float32 PCM through the SoundAdapter, with
  format, sessionId, tag, and PTS. The game's own mixer passes it to cellAudio.
  Player completion waits for the final video and audio frames to be delivered.
- `audio_bridge.c` retains the SDK audio HLE and fixes POSIX thread pacing on
  Linux: it follows the device instead of consuming blocks at a fixed interval.
  `AUDIO_WAV=/path/to/mix.f32le` records the final stereo output at 48 kHz;
  `AUDIO_PEAK=1` logs its peak level for diagnostics.

## Validation

```bash
cmake --build --preset linux-vulkan
bash tests/test_sail.sh
SANITIZE=1 bash tests/test_sail.sh
SANITIZE=1 bash tests/test_sail_audio.sh
PORT_TEST_SECONDS=300 ./scripts/run-vulkan.sh
```

The test uses a synthetic six-frame video. It checks deferred callbacks,
64-bit tokens, state and path validation, allocation errors, actual frames,
pause, end of file, explicit Stop, cancellation, and decoder failure.
SANITIZE mode uses AddressSanitizer and UndefinedBehaviorSanitizer; leak
detection is disabled because the application environment prevents inspection.
The audio test uses the original PAMF: it validates 6,533,120 samples per channel,
nonzero energy, finite values, cancellation, and truncated frame rejection.

## Limitations

This implements the imports used by BLJS10220, rather than the complete cellSail
API. Audio demuxing accepts the first mono/stereo ATRAC3plus track at 48 kHz,
with up to 32 MiB of compressed data. Other audio codecs, seeking, looping, and
some image formats remain unimplemented. Unsupported playback options return
an error. PTS conversion and pacing use the constant frame rate found by probing.

Component tests reached the title screen displaying “PRESS ○ BUTTON”.
The start of the mission was validated later; see the README for current status.
The `analysis/sail-av-*` and `analysis/sail-video-*` directories contain local
logs excluded from publication. They are not included in the source package.

For diagnostics, `PORT_SAIL_TRACE=1` logs the internal player states used by
this game. `PORT_SAIL_TEST_MOVIE=/path/to/file.pam` replaces only this run's
video source with a separate fixture, allowing quick playback completion tests.
Neither option is enabled in the normal launcher; the second is exclusively
for testing and does not modify the original game files.
