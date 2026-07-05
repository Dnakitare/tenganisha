# HANDOFF — Tenganisha VST3 stem separator

**Mission:** take this project from "architecturally complete, header-verified" to "compiles clean, passes pluginval strictness 10, separates a real song in a real DAW." Do not redesign; fix forward. Read README.md first for the architecture rationale.

## Current state

- Full JUCE 8 CMake project: processor (state machine, lock-free stem playback), separation engine (background thread wrapping demucs.cpp), editor (transport + 4 stem strips + drag-to-DAW export).
- Every demucs.cpp API call was verified against the actual headers at `sevagh/demucs.cpp` main (July 2026): `load_demucs_model(std::string, demucs_model*)`, `demucs_inference(model, Eigen::MatrixXf(2,N) @44100, ProgressCallback) -> Eigen::Tensor3dXf(sources, 2, N)`, stem order drums/bass/other/vocals, `model.is_4sources`, `demucscpp::SUPPORTED_SAMPLE_RATE`.
- **Never compiled.** Expect and fix the friction below.

## Phase 1 — Get it building (highest priority)

1. `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
2. Known likely breakages, in probability order:
   - **`FetchContent_Populate` is deprecated** (CMake ≥ 3.30). Replace with `FetchContent_MakeAvailable` + `SOURCE_SUBDIR` tricks or `EXCLUDE_FROM_ALL`; for Eigen you only need the source dir on the include path — do NOT run Eigen's or demucs.cpp's own CMake.
   - **JUCE 8 deprecations:** `juce::Font(float, int)` ctor → `juce::FontOptions`; check `Thread::Priority` enum usage; `juce::ProgressBar` ctor takes `double&` (already correct but verify lifetime).
   - **demucs.cpp under strict warnings:** it's built with `-Wall -Wextra` upstream but our JUCE recommended-warning flags may differ; add `target_compile_options` suppressions to the `demucs_inference` target only, never to plugin code.
   - **Eigen unsupported modules:** demucs.cpp includes `<unsupported/Eigen/...>`; the FetchContent'd Eigen source tree contains `unsupported/` at the root — confirm include path covers it.
   - **`std::atomic_load/store(shared_ptr)`**: fine in C++17, deprecated in C++20. If the toolchain forces C++20, migrate to `std::atomic<std::shared_ptr<...>>`.
3. macOS: build both `arm64` and `x86_64` at least once. Windows: use clang-cl (demucs.cpp is not MSVC-tested).

## Phase 2 — Correctness gates

1. **pluginval** (Tracktion) at strictness level 10, all formats. Fix everything it flags; typical suspects here:
   - allocation/locks on audio thread (there should be none — verify the `stemPlayback` path),
   - editor deletion race with the 15 Hz timer,
   - state save/recall roundtrip.
2. **Standalone smoke test:** build the Standalone target, load `ggml-model-htdemucs-4s-f16.bin` (`scripts/download_model.sh`), feed a 30 s music WAV through (loopback or add a temporary file-player), confirm 4 stems that sum ≈ input (null test within model error, not bit-exact).
3. **Offline correctness:** stems summed with all gains at 0 dB should reconstruct the mix closely. Automate: render input vs. stem-sum, report RMS of difference.

## Phase 3 — DAW validation

- Reaper (fastest to script): insert on a track, record 20 s, separate, confirm playback lands sample-aligned when looping/relocating the playhead. Edge cases: playhead jump mid-block, loop boundary, host stop during `recording` state, offline bounce while in `stemPlayback`.
- Verify drag-a-stem-strip drops a valid 24-bit WAV onto a track (macOS Finder + Reaper, Windows Explorer + Reaper).

## Phase 4 — Performance (only after 1–3 pass)

- The per-sample inner loop in `processBlock`'s stem mix does a bounds check + per-sample smoothed gain per stem. Restructure to block-wise: clamp the copy range once, use `AudioBuffer::addFromWithRamp` or apply smoothing per-block. Target: < 1% CPU during stem playback at 128-sample buffers.
- Inference throughput: try OpenMP thread count tuning; optionally wire demucs.cpp's `EIGEN_USE_BLAS` path with OpenBLAS as a CMake option.

## Phase 5 — Roadmap (do not start unless 1–4 are green)

1. Waveform display of capture with per-stem colored overlays post-separation.
2. File-drop mode: drag a WAV *into* the plugin as an alternative to timeline capture.
3. `htdemucs_ft` fine-tuned 4-model ensemble as a "max quality" mode (4× inference time).
4. ARA2 integration — stems as editable regions. This is the long-term differentiator; spike only.

## Constraints & conventions

- Never allocate, lock, or log on the audio thread.
- Keep Eigen/demucs headers confined to `SeparationEngine.cpp` (pimpl already in place) — JUCE TU compile times matter.
- All user-visible strings stay in the editor; processor stays UI-free.
- Name "Tenganisha" is provisional — pending trademark/prior-art sweep. Keep it greppable (single token).

## Definition of done

- [x] Clean Release build on macOS (arm64) with zero warnings in `src/` (2026-07-05; also universal arm64+x86_64, x86 slice pluginval-validated under Rosetta)
- [x] pluginval strictness 10 pass, VST3 + AU (both SUCCESS; auval PASS)
- [x] Null-ish reconstruction test scripted and passing (`tenganisha_offline_test`: -34.7 dB on real music; plus `tenganisha_host_sim_test`: sample-exact alignment, -132 dB vs stem sum)
- [x] Reaper checklist (2026-07-05, automated via ReaScript + macOS accessibility):
  - passthrough render null-tests at -93 dB RMS vs input
  - live record off the timeline → separate → stems land sample-aligned: offline bounce of stemPlayback nulls vs input at -43.7 dB (model error, not misalignment)
  - realtime looped playback: laps bit-consistent (-51 dB lap-to-lap), match the offline render at -48.6 dB and the input at -45.6 dB (a 77.5 ms constant is REAPER's output-record latency, not the plugin)
  - drag-a-stem-strip → valid 24-bit/44.1k stereo WAV lands on a REAPER track
  - found+fixed live: hosts run FX while the transport is stopped (REAPER anticipative processing runs it faster than realtime); capture now only ingests blocks where the host reports playing, else the take is stuffed with silence and misaligned (was -13 dB residual before fix)
  - still human: subjective separation quality listening; drag-to-Finder
- [ ] README build instructions verified from scratch on a clean machine/container
