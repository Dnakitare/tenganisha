// Offline correctness gate: drive the real SeparationEngine end-to-end and
// verify the stems summed at unity reconstruct the input (null test within
// model error). Exits 0 iff reconstruction residual is below threshold.
//
// Usage: tenganisha_offline_test <model.bin> [input.wav] [out_dir]
// With no input.wav a 30 s synthetic "song" (drums/bass/chords/voice-ish) is
// generated so the test is self-contained.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include "SeparationEngine.h"

#include <atomic>
#include <cmath>
#include <cstdio>

namespace
{

juce::AudioBuffer<float> makeTestSong (double rate, double seconds)
{
    const int n = (int) (rate * seconds);
    juce::AudioBuffer<float> b (2, n);
    b.clear();

    juce::Random rng (0x7e46a);
    const double twoPi = juce::MathConstants<double>::twoPi;

    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate;

        // bass: root-note square-ish sine, 55 Hz, eighth-note gate
        const bool bassOn = std::fmod (t, 0.5) < 0.4;
        const float bass = bassOn ? 0.30f * (float) std::sin (twoPi * 55.0 * t) : 0.0f;

        // "chords": detuned saw pair around 220/277/330 Hz
        float chord = 0.0f;
        for (double f : { 220.0, 277.18, 329.63 })
            chord += 0.06f * (float) (2.0 * std::fmod (f * t, 1.0) - 1.0);

        // drums: kick (decaying 60 Hz burst on the beat) + hat (noise ticks)
        const double beatT = std::fmod (t, 0.5);
        const float kick = beatT < 0.12
            ? 0.5f * (float) (std::sin (twoPi * 60.0 * beatT) * std::exp (-beatT * 40.0))
            : 0.0f;
        const double hatT = std::fmod (t + 0.25, 0.5);
        const float hat = hatT < 0.03
            ? 0.15f * (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-hatT * 200.0)
            : 0.0f;

        // voice-ish: vibrato sine melody, sustained
        const double f0 = 440.0 * std::pow (2.0, std::floor (std::fmod (t, 4.0)) / 12.0);
        const float voice = 0.12f * (float) std::sin (twoPi * f0 * t + 4.0 * std::sin (twoPi * 5.5 * t));

        const float sL = bass + chord + kick + hat + voice;
        const float sR = bass + 0.8f * chord + kick + 1.2f * hat + voice;
        b.setSample (0, i, sL);
        b.setSample (1, i, sR);
    }
    return b;
}

double rmsOfDifference (const juce::AudioBuffer<float>& a,
                        const juce::AudioBuffer<float>& b, int n)
{
    double acc = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const double d = a.getSample (ch, i) - b.getSample (ch, i);
            acc += d * d;
        }
    return std::sqrt (acc / (2.0 * n));
}

double rmsOf (const juce::AudioBuffer<float>& a, int n)
{
    double acc = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < n; ++i)
        {
            const double v = a.getSample (ch, i);
            acc += v * v;
        }
    return std::sqrt (acc / (2.0 * n));
}

void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double rate)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    if (auto stream = f.createOutputStream())
    {
        std::unique_ptr<juce::AudioFormatWriter> w (
            fmt.createWriterFor (stream.get(), rate, 2, 24, {}, 0));
        if (w != nullptr)
        {
            stream.release();
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
    }
}

// Pump the message loop until `done` flips or timeout. Callbacks from the
// engine arrive via MessageManager::callAsync, so we must dispatch.
bool pumpUntil (std::atomic<bool>& done, int timeoutMs)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! done.load())
    {
        if (juce::Time::getMillisecondCounter() > deadline) return false;
        mm->runDispatchLoopUntil (50);
    }
    return true;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: %s <model.bin> [input.wav] [out_dir]\n", argv[0]);
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File modelFile (juce::File::getCurrentWorkingDirectory()
                                    .getChildFile (juce::String (argv[1])));
    const double rate = 44100.0;

    // ---- input ----
    juce::AudioBuffer<float> input;
    if (argc >= 3)
    {
        juce::AudioFormatManager afm;
        afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            afm.createReaderFor (juce::File::getCurrentWorkingDirectory()
                                     .getChildFile (juce::String (argv[2]))));
        if (reader == nullptr || reader->sampleRate <= 0)
        {
            std::printf ("FAIL: cannot read input wav\n");
            return 2;
        }
        input.setSize (2, (int) reader->lengthInSamples);
        reader->read (&input, 0, (int) reader->lengthInSamples, 0, true, true);
        if ((int) reader->sampleRate != (int) rate)
            std::printf ("note: input is %d Hz, engine will resample\n",
                         (int) reader->sampleRate);
    }
    else
    {
        input = makeTestSong (rate, 30.0);
        std::printf ("generated 30 s synthetic test song\n");
    }

    const juce::File outDir = argc >= 4
        ? juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[3]))
        : juce::File::getSpecialLocation (juce::File::tempDirectory)
              .getChildFile ("tenganisha-test");
    outDir.createDirectory();

    // ---- engine ----
    tg::SeparationEngine engine;

    std::atomic<bool> loaded { false }, loadOk { false };
    engine.onModelLoaded = [&] { loadOk = true; loaded = true; };
    engine.onError       = [&] (juce::String e)
    {
        std::printf ("engine error: %s\n", e.toRawUTF8());
        loaded = true;
    };

    engine.loadModelAsync (modelFile);
    if (! pumpUntil (loaded, 120'000) || ! loadOk)
    {
        std::printf ("FAIL: model load\n");
        return 1;
    }
    std::printf ("model loaded: %s\n", modelFile.getFileName().toRawUTF8());

    std::atomic<bool> separated { false };
    tg::StemSetPtr stems;
    engine.onStemsReady = [&] (tg::StemSetPtr s) { stems = s; separated = true; };
    engine.onError      = [&] (juce::String e)
    {
        std::printf ("engine error: %s\n", e.toRawUTF8());
        separated = true;
    };

    juce::AudioBuffer<float> inputCopy (input); // engine takes ownership
    const auto t0 = juce::Time::getMillisecondCounter();
    engine.separateAsync (std::move (inputCopy), rate, 0);

    // budget: model is roughly realtime x1-4 on CPU; allow x20
    const int budgetMs = (int) (20.0 * 1000.0 * input.getNumSamples() / rate) + 60'000;
    if (! pumpUntil (separated, budgetMs) || stems == nullptr)
    {
        std::printf ("FAIL: separation did not complete\n");
        return 1;
    }
    const double elapsed = (juce::Time::getMillisecondCounter() - t0) / 1000.0;
    std::printf ("separated %.1f s of audio in %.1f s (x%.2f realtime)\n",
                 input.getNumSamples() / rate, elapsed,
                 elapsed / (input.getNumSamples() / rate));

    // ---- reconstruction ----
    const int n = juce::jmin (input.getNumSamples(), stems->numSamples());
    juce::AudioBuffer<float> sum (2, n);
    sum.clear();
    for (const auto& stem : stems->stems)
        for (int ch = 0; ch < 2; ++ch)
            sum.addFrom (ch, 0, stem, ch, 0, n);

    const double inRms  = rmsOf (input, n);
    const double residual = rmsOfDifference (input, sum, n);
    const double residualDb = juce::Decibels::gainToDecibels (residual / juce::jmax (1e-12, inRms));

    std::printf ("samples compared: %d (input %d, stems %d)\n",
                 n, input.getNumSamples(), stems->numSamples());
    std::printf ("input RMS: %.6f, residual RMS: %.6f, residual: %.1f dB\n",
                 inRms, residual, residualDb);

    for (int s = 0; s < tg::kNumStems; ++s)
        writeWav (outDir.getChildFile (juce::String (tg::stemName (s)).toLowerCase() + ".wav"),
                  stems->stems[(size_t) s], rate);
    writeWav (outDir.getChildFile ("input.wav"), input, rate);
    writeWav (outDir.getChildFile ("stem_sum.wav"), sum, rate);
    std::printf ("wavs written to %s\n", outDir.getFullPathName().toRawUTF8());

    // Threshold: htdemucs stems are not a perfect partition of the input, but
    // the residual should sit well below the signal. -20 dB is a loose but
    // meaningful gate; typical results are far better on real music.
    if (residualDb > -20.0)
    {
        std::printf ("FAIL: residual above -20 dB gate\n");
        return 1;
    }
    std::printf ("PASS\n");
    return 0;
}
