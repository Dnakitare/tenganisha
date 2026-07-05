#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>

namespace tg
{

enum class Stem : int { drums = 0, bass = 1, other = 2, vocals = 3 };
constexpr int kNumStems = 4;

inline const char* stemName (int i)
{
    static const char* names[] = { "Drums", "Bass", "Other", "Vocals" };
    return names[i];
}

// Immutable once published. Audio thread takes a shared_ptr snapshot per
// block; the engine thread builds a fresh StemSet and swaps the pointer.
struct StemSet
{
    // stems[i]: stereo buffer at *host* sample rate, all identical length
    std::array<juce::AudioBuffer<float>, kNumStems> stems;
    double sampleRate = 44100.0;
    // Host timeline sample position (at host rate) where capture began,
    // so playback can be aligned to the original material.
    juce::int64 timelineStart = 0;

    int numSamples() const { return stems[0].getNumSamples(); }
};

using StemSetPtr = std::shared_ptr<const StemSet>;

enum class EngineState : int
{
    passthrough = 0,   // idle: input -> output untouched
    recording,         // capturing host audio
    separating,        // background inference running
    stemPlayback       // stems replace input, aligned to timeline
};

} // namespace tg
