#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>
#include "StemSet.h"
#include <atomic>
#include <functional>
#include <memory>

struct demucs_model_holder; // pimpl: keeps Eigen/demucs headers out of JUCE TUs

namespace tg
{

// Owns the model and runs inference off the audio thread.
// Lifecycle: loadModel() (async) -> separate() (async) -> onStemsReady.
class SeparationEngine : private juce::Thread
{
public:
    SeparationEngine();
    ~SeparationEngine() override;

    // ---- control (call from message thread) ----
    void loadModelAsync (const juce::File& ggmlWeights);

    // audio: stereo, host sample rate. timelineStart: host-timeline sample
    // position where this capture began.
    void separateAsync (juce::AudioBuffer<float> audio,
                        double hostSampleRate,
                        juce::int64 timelineStart);

    void cancel();

    // ---- observation ----
    bool  isModelLoaded()  const { return modelLoaded.load(); }
    bool  isBusy()         const { return busy.load(); }
    float getProgress()    const { return progress.load(); }   // 0..1
    juce::String getStatus() const;

    // Fired on the message thread.
    std::function<void (StemSetPtr)>       onStemsReady;
    std::function<void (juce::String)>     onError;
    std::function<void()>                  onModelLoaded;

private:
    void run() override;

    enum class Job { none, load, separate };

    std::unique_ptr<demucs_model_holder> model;
    std::atomic<bool>  modelLoaded { false };
    std::atomic<bool>  busy        { false };
    std::atomic<float> progress    { 0.0f };
    std::atomic<int>   pendingJob  { (int) Job::none };

    juce::CriticalSection jobLock;   // guards the fields below (not audio-thread)
    juce::File            weightsFile;
    juce::AudioBuffer<float> jobAudio;
    double                jobRate = 44100.0;
    juce::int64           jobTimelineStart = 0;
    juce::String          statusText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SeparationEngine)
};

} // namespace tg
