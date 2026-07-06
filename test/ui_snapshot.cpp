// Renders the editor offscreen to PNGs, one per engine state, so UI work can
// be reviewed without a windowing session. Also handy for README screenshots.
//
// Usage: tenganisha_ui_snapshot <out_dir> [model.bin]
// Without a model: only the "no model" state. With one: ready, recording and
// stems-live states too (runs a real 2 s separation).

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "PluginProcessor.h"

#include <atomic>
#include <cstdio>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    void pumpMessages (int ms)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }

    void snapshot (juce::AudioProcessorEditor& ed, const juce::File& file)
    {
        pumpMessages (300); // let timers settle state-driven visuals
        auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.0f);
        juce::PNGImageFormat png;
        file.deleteFile();
        if (auto stream = file.createOutputStream())
            png.writeImageToStream (img, *stream);
        std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
    }
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: %s <out_dir> [model.bin]\n", argv[0]);
        return 2;
    }

    // Deterministic states: don't let the editor auto-load installed models.
   #if ! JUCE_WINDOWS
    setenv ("TENGANISHA_NO_AUTOLOAD", "1", 1);
   #endif

    juce::ScopedJuceInitialiser_GUI juceInit;

    const auto outDir = juce::File::getCurrentWorkingDirectory()
                            .getChildFile (juce::String (argv[1]));
    outDir.createDirectory();

    std::unique_ptr<juce::AudioProcessor> plugin (createPluginFilter());
    auto& proc = dynamic_cast<tg::TenganishaProcessor&> (*plugin);
    proc.setPlayConfigDetails (2, 2, 44100.0, 512);
    proc.prepareToPlay (44100.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());

    snapshot (*ed, outDir.getChildFile ("state-no-model.png"));

    if (argc >= 3)
    {
        std::atomic<bool> loaded { false }, ok { false };
        proc.engine.onModelLoaded = [&] { ok = true; loaded = true; };
        proc.engine.onError = [&] (juce::String) { loaded = true; };
        proc.engine.loadModelAsync (juce::File::getCurrentWorkingDirectory()
                                        .getChildFile (juce::String (argv[2])));
        while (! loaded.load()) pumpMessages (50);
        if (! ok.load()) { std::printf ("model load failed\n"); return 1; }

        snapshot (*ed, outDir.getChildFile ("state-ready.png"));

        // recording (no transport in this harness: shows the press-play hint)
        proc.startRecording();
        snapshot (*ed, outDir.getChildFile ("state-recording.png"));

        // feed 2 s of tone through the block path, then separate for real
        juce::AudioBuffer<float> blk (2, 512);
        juce::MidiBuffer midi;
        for (int i = 0; i < (int) (2.0 * 44100 / 512); ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < 512; ++n)
                    blk.setSample (ch, n, 0.25f * std::sin (0.05f * (float) (i * 512 + n)));
            proc.processBlock (blk, midi);
        }
        proc.stopRecordingAndSeparate();
        while (proc.getEngineState() == tg::EngineState::separating)
            pumpMessages (100);

        snapshot (*ed, outDir.getChildFile ("state-stems-live.png"));
    }

    ed.reset();
    proc.releaseResources();
    return 0;
}
