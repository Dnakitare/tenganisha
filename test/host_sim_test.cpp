// Phase 3 (automated half): drive the real TenganishaProcessor with a
// scripted host playhead and verify the state machine plus sample-accurate
// timeline alignment of stem playback, including the edge cases from the
// handoff: playhead jump mid-session, loop boundary, host stop while
// recording.
//
// Usage: tenganisha_host_sim_test <model.bin>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "PluginProcessor.h"

#include <atomic>
#include <cmath>
#include <cstdio>

// The plugin's factory (defined in PluginProcessor.cpp).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{

constexpr double kRate = 44100.0;
constexpr int    kBlock = 512;

struct ScriptedPlayHead : juce::AudioPlayHead
{
    juce::int64 pos = 0;
    bool playing = true;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setTimeInSamples (pos);
        info.setIsPlaying (playing);
        return info;
    }
};

int failures = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (! ok) ++failures;
}

// Deterministic "music": distinct tone per timeline position so alignment
// errors show up as signal mismatch, not silence.
float srcSample (juce::int64 timelinePos)
{
    const double t = (double) timelinePos / kRate;
    return 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * t)
         + 0.2f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 3.7 * t);
}

void pumpMessages (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: %s <model.bin>\n", argv[0]);
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    std::unique_ptr<juce::AudioProcessor> plugin (createPluginFilter());
    auto& proc = dynamic_cast<tg::TenganishaProcessor&> (*plugin);

    ScriptedPlayHead ph;
    proc.setPlayHead (&ph);
    proc.setPlayConfigDetails (2, 2, kRate, kBlock);
    proc.prepareToPlay (kRate, kBlock);

    juce::AudioBuffer<float> buf (2, kBlock);
    juce::MidiBuffer midi;

    auto processAt = [&] (juce::int64 timelinePos, bool fillInput = true)
    {
        ph.pos = timelinePos;
        if (fillInput)
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = srcSample (timelinePos + i);
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
        else
            buf.clear();
        proc.processBlock (buf, midi);
    };

    // ---- model load ----
    std::atomic<bool> loaded { false }, loadOk { false };
    proc.engine.onModelLoaded = [&] { loadOk = true; loaded = true; };
    proc.engine.onError = [&] (juce::String) { loaded = true; };
    proc.engine.loadModelAsync (juce::File::getCurrentWorkingDirectory()
                                    .getChildFile (juce::String (argv[1])));
    while (! loaded.load()) pumpMessages (50);
    check (loadOk.load(), "model loads");

    // ---- test 1: host stop during recording, then resume ----
    // (recording keeps appending whenever blocks arrive; host stop just means
    // no blocks / not playing. Nothing should crash or corrupt state.)
    const juce::int64 recStart = 123'456; // arbitrary non-zero timeline start
    proc.startRecording();
    check (proc.getEngineState() == tg::EngineState::recording, "enters recording");

    // Record armed while the host transport is stopped: REAPER et al. keep
    // calling processBlock (position parked, isPlaying false). None of this
    // may land in the capture or stamp the timeline start.
    ph.playing = false;
    for (int b = 0; b < 40; ++b)
        processAt (recStart - 30'000, false);
    check (proc.getRecordedSeconds() == 0.0, "stopped-transport blocks are not captured");
    ph.playing = true;

    const int recBlocks = (int) std::ceil (8.0 * kRate / kBlock); // ~8 s
    for (int b = 0; b < recBlocks; ++b)
    {
        processAt (recStart + (juce::int64) b * kBlock);
        if (b == recBlocks / 2)
        {
            // host stop mid-recording: a few not-playing, empty-position blocks
            ph.playing = false;
            processAt (recStart + (juce::int64) b * kBlock, false);
            ph.playing = true;
        }
    }
    check (proc.getRecordedSeconds() > 7.9, "capture length ≈ 8 s");

    // ---- separate ----
    // NOTE: the processor snapshots+hands off on the *message* thread here,
    // matching how the editor's Separate button calls it.
    proc.stopRecordingAndSeparate();
    check (proc.getEngineState() == tg::EngineState::separating, "enters separating");

    while (proc.getEngineState() == tg::EngineState::separating) pumpMessages (100);
    check (proc.getEngineState() == tg::EngineState::stemPlayback, "stems ready -> stemPlayback");

    auto stems = proc.getStems();
    check (stems != nullptr, "stems published");

    // ---- test 2: aligned playback reconstructs the source at the same
    //      timeline position (after gain smoothing settles) ----
    // Play 2 s from 1 s into the captured region.
    // First run 0.5 s to let the 20 ms gain smoothing settle.
    juce::int64 playPos = recStart + (juce::int64) kRate;
    const int settleBlocks = (int) (0.5 * kRate / kBlock);
    for (int b = 0; b < settleBlocks; ++b) { processAt (playPos); playPos += kBlock; }

    // Gate on output vs the stem *sum* at the same timeline offset — that
    // isolates alignment from model reconstruction error. Any off-by-N
    // indexing turns this into a large comb-filter residual.
    double err = 0, ref = 0, errSrc = 0, refSrc = 0;
    const int measureBlocks = (int) (2.0 * kRate / kBlock);
    for (int b = 0; b < measureBlocks; ++b)
    {
        processAt (playPos);
        for (int i = 0; i < kBlock; ++i)
        {
            const auto idx = (int) (playPos + i - stems->timelineStart);
            double want = 0;
            for (const auto& st : stems->stems)
                want += st.getSample (0, idx);
            const double got = buf.getSample (0, i);
            err += (want - got) * (want - got);
            ref += want * want;

            const double src = srcSample (playPos + i);
            errSrc += (src - got) * (src - got);
            refSrc += src * src;
        }
        playPos += kBlock;
    }
    const double alignedDb = 10.0 * std::log10 (err / juce::jmax (1e-30, ref));
    const double vsSrcDb   = 10.0 * std::log10 (errSrc / juce::jmax (1e-30, refSrc));
    std::printf ("output vs stem-sum: %.1f dB, vs source (model error incl.): %.1f dB\n",
                 alignedDb, vsSrcDb);
    check (alignedDb < -60.0, "playback is sample-aligned to stem sum (< -60 dB)");

    // ---- test 3: loop boundary / playhead jump mid-block ----
    // Jump back to the same position: output must be identical to what the
    // same timeline position produced before (deterministic indexing).
    juce::AudioBuffer<float> first (2, kBlock), second (2, kBlock);
    const juce::int64 loopPos = recStart + (juce::int64) (2.5 * kRate);

    processAt (loopPos);
    for (int ch = 0; ch < 2; ++ch) first.copyFrom (ch, 0, buf, ch, 0, kBlock);

    // wander off, then loop back
    for (int b = 0; b < 40; ++b) processAt (loopPos + 40'000 + (juce::int64) b * kBlock);
    processAt (loopPos);
    for (int ch = 0; ch < 2; ++ch) second.copyFrom (ch, 0, buf, ch, 0, kBlock);

    double loopDiff = 0;
    for (int i = 0; i < kBlock; ++i)
        loopDiff = juce::jmax (loopDiff, (double) std::abs (first.getSample (0, i) - second.getSample (0, i)));
    std::printf ("loop-return max sample diff: %.6f\n", loopDiff);
    check (loopDiff < 1.0e-3, "loop/relocate returns identical audio");

    // ---- test 4: playback outside the captured region is silent ----
    processAt (recStart - 10 * kBlock);
    check (buf.getMagnitude (0, kBlock) < 1.0e-4, "before capture region: silence");
    processAt (recStart + (juce::int64) (600.0 * kRate));
    check (buf.getMagnitude (0, kBlock) < 1.0e-4, "far after capture region: silence");

    // ---- test 5: discard returns to passthrough ----
    proc.discardStems();
    check (proc.getEngineState() == tg::EngineState::passthrough, "discard -> passthrough");
    check (proc.getStems() == nullptr, "stems released");
    processAt (recStart);
    check (std::abs (buf.getSample (0, 0) - srcSample (recStart)) < 1.0e-6,
           "passthrough leaves input untouched");

    proc.releaseResources();
    proc.setPlayHead (nullptr);

    std::printf (failures == 0 ? "PASS\n" : "FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
