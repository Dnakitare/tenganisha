// Verifies the model-library plumbing without any UI or network:
//  - canonical directory + filename mapping
//  - isInstalled reflects real files
//  - creating the editor auto-loads the model in the canonical dir
//  - model path persists through getStateInformation/setStateInformation
//
// Usage: tenganisha_model_manager_test  (uses whatever is installed)

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ModelLibrary.h"

#include <atomic>
#include <cstdio>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace { int failures = 0;
void check (bool ok, const char* what)
{
    std::printf ("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (! ok) ++failures;
}}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    using namespace tg;

    check (models::primaryFile (models::Kind::standard).getFileName()
               == "ggml-model-htdemucs-4s-f16.bin", "standard primary filename");
    check (models::filenames (models::Kind::fineTuned).size() == 4, "ft set has 4 files");
    check (models::directory().getFullPathName().contains ("Tenganisha"),
           "canonical dir under app support");

    std::printf ("canonical dir: %s\n", models::directory().getFullPathName().toRawUTF8());
    auto pf = models::primaryFile (models::Kind::standard);
    std::printf ("primary: %s exists=%d size=%lld\n", pf.getFullPathName().toRawUTF8(),
                 (int) pf.existsAsFile(), (long long) pf.getSize());
    const bool haveStandard = models::isInstalled (models::Kind::standard);
    std::printf ("standard installed: %s\n", haveStandard ? "yes" : "no");

    // ---- editor auto-loads the installed model ----
    {
        std::unique_ptr<juce::AudioProcessor> plugin (createPluginFilter());
        auto& proc = dynamic_cast<TenganishaProcessor&> (*plugin);
        proc.setPlayConfigDetails (2, 2, 44100.0, 512);
        proc.prepareToPlay (44100.0, 512);

        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        // wait for the async load kicked off in the editor ctor
        for (int i = 0; i < 200 && ! proc.engine.isModelLoaded(); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        if (haveStandard)
        {
            check (proc.engine.isModelLoaded(), "editor auto-loaded installed model");
            check (proc.getModelPath() == models::primaryFile (models::Kind::standard).getFullPathName(),
                   "model path recorded on auto-load");
        }
        else
        {
            check (! proc.engine.isModelLoaded(), "no model loaded when none installed");
        }
        ed.reset();
    }

    // ---- model path survives a state save/restore roundtrip ----
    {
        std::unique_ptr<juce::AudioProcessor> a (createPluginFilter());
        auto& pa = dynamic_cast<TenganishaProcessor&> (*a);
        pa.setModelPath ("/some/custom/path/model.bin");
        juce::MemoryBlock blob;
        pa.getStateInformation (blob);

        std::unique_ptr<juce::AudioProcessor> b (createPluginFilter());
        auto& pb = dynamic_cast<TenganishaProcessor&> (*b);
        pb.setStateInformation (blob.getData(), (int) blob.getSize());
        check (pb.getModelPath() == "/some/custom/path/model.bin",
               "model path persists through state roundtrip");
    }

    std::printf (failures == 0 ? "PASS\n" : "FAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
