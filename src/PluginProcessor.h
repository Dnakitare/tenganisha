#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "StemSet.h"
#include "SeparationEngine.h"

namespace tg
{

class TenganishaProcessor : public juce::AudioProcessor
{
public:
    TenganishaProcessor();
    ~TenganishaProcessor() override;

    //==================================================== AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                  { return true; }
    const juce::String getName() const override      { return "Tenganisha"; }
    bool acceptsMidi() const override                { return false; }
    bool producesMidi() const override               { return false; }
    double getTailLengthSeconds() const override     { return 0.0; }
    int getNumPrograms() override                    { return 1; }
    int getCurrentProgram() override                 { return 0; }
    void setCurrentProgram (int) override            {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==================================================== Tenganisha API
    void startRecording();          // message thread
    void stopRecordingAndSeparate();
    void discardStems();            // back to passthrough

    EngineState getEngineState() const { return (EngineState) engineState.load(); }
    StemSetPtr  getStems() const       { return std::atomic_load (&currentStems); }
    double      getRecordedSeconds() const;

    SeparationEngine engine;
    juce::AudioProcessorValueTreeState apvts;

    static constexpr double kMaxCaptureSeconds = 600.0; // 10 min

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    // ---- capture (audio thread writes, engine reads after handoff) ----
    // captureBuffer is allocated lazily in startRecording() (message thread):
    // 10 min of stereo floats is ~220 MB and must not be paid per idle instance.
    juce::AudioBuffer<float>  captureBuffer;
    std::atomic<int>          captureLength { 0 };
    std::atomic<juce::int64>  captureTimelineStart { 0 }; // audio thread writes, message thread reads

    // ---- playback ----
    StemSetPtr currentStems;                 // atomic shared_ptr swap
    std::atomic<int> engineState { (int) EngineState::passthrough };
    juce::int64 internalPos = 0;             // fallback when host gives no playhead

    // cached param pointers (audio-thread safe)
    std::array<std::atomic<float>*, kNumStems> gainParams {};
    std::array<std::atomic<float>*, kNumStems> muteParams {};
    std::array<std::atomic<float>*, kNumStems> soloParams {};
    std::atomic<float>* masterParam = nullptr;

    // per-stem smoothed gain to avoid zipper noise
    std::array<juce::SmoothedValue<float>, kNumStems> smoothedGain;

    double hostRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TenganishaProcessor)
};

} // namespace tg
