#pragma once
#include <juce_core/juce_core.h>

// Known model sets and their canonical install location, so users pick
// "Standard" or "Fine-tuned" instead of hunting for .bin files.
namespace tg::models
{

enum class Kind { standard, fineTuned };

inline juce::File directory()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    // On macOS this special location is ~/Library; the conventional app-data
    // home is the Application Support subfolder.
    base = base.getChildFile ("Application Support");
   #endif
    return base.getChildFile ("Tenganisha").getChildFile ("models");
}

inline const char* baseUrl()
{
    return "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/";
}

inline juce::StringArray filenames (Kind k)
{
    if (k == Kind::standard)
        return { "ggml-model-htdemucs-4s-f16.bin" };
    return { "ggml-model-htdemucs_ft_drums-4s-f16.bin",
             "ggml-model-htdemucs_ft_bass-4s-f16.bin",
             "ggml-model-htdemucs_ft_other-4s-f16.bin",
             "ggml-model-htdemucs_ft_vocals-4s-f16.bin" };
}

// The file handed to the engine; for the fine-tuned set any member selects
// the whole ensemble (siblings are found next to it).
inline juce::File primaryFile (Kind k)
{
    return directory().getChildFile (filenames (k)[0]);
}

inline bool isInstalled (Kind k)
{
    for (const auto& name : filenames (k))
    {
        auto f = directory().getChildFile (name);
        if (! f.existsAsFile() || f.getSize() < 1024 * 1024)
            return false;
    }
    return true;
}

} // namespace tg::models
