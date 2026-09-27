# Chromaprint (vendored)

Vendored source of **Chromaprint 1.6.1** — <https://github.com/acoustid/chromaprint>
— the audio fingerprinting library behind AcoustID, imported so that mousiki
*builds* `fpcalc` (`tools/fpcalc.cpp`) instead of shipping a prebuilt binary:
configuring and building the project needs no download at any point.

Chromaprint's own sources are MIT; upstream's `LICENSE.md` notes that the FFmpeg
fragments it carries are LGPL-2.1, so treat the project as LGPL-2.1 as a whole.
The bundled kissfft is under `kissfft/COPYING` (BSD-3-Clause / Unlicense).

## What is in here

- The library exactly as upstream's `src/CMakeLists.txt` lists it
  (`chromaprint.cpp` + the algorithm sources + `utils/base64.cpp`), plus the
  headers they include and `utils/`' helpers.
- `kissfft/` — Chromaprint's *own* bundled copy, kept separate on purpose:
  `fft_lib_kissfft.cpp` is written against this version, and it differs from
  `third_party/kissfft`, which serves the spectrum visualizer. Both are compiled
  into different executables, so the two never meet at link time.
- `config.h` — replaces upstream's CMake-generated header (see the file).

## What was left out

- `cmd/fpcalc.cpp` — upstream's tool *links* FFmpeg (libavformat/libavcodec/
  libswresample) to decode audio, which would mean vendoring a whole FFmpeg
  development build. mousiki already requires `ffmpeg`/`ffprobe` at runtime
  (tag writing, Opus playback, metadata), so `tools/fpcalc.cpp` spawns `ffmpeg`
  and reads raw PCM from its stdout instead — everything downstream of the PCM
  is stock Chromaprint.
- The other FFT backends (`fft_lib_avtx/avfft/fftw3/vdsp.cpp`) and the FFmpeg
  audio plumbing (`audio/ffmpeg_*.h`, `avresample/`) — same reason.
- The test suite and `3rdparty/googletest/`.

Upstream release archive: `chromaprint-1.6.1.tar.gz` / tag `v1.6.1`.
