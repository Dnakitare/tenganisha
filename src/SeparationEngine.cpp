#include "SeparationEngine.h"
#include <juce_audio_processors/juce_audio_processors.h>

// demucs.cpp — Eigen-heavy, keep confined to this TU.
#include "model.hpp"
#include "dsp.hpp"

struct demucs_model_holder
{
    // 1 entry: standard htdemucs (all stems from one model).
    // 4 entries: htdemucs_ft ensemble, models[i] specialises stem i
    // (drums, bass, other, vocals) — 4x inference for better quality.
    std::vector<std::unique_ptr<demucscpp::demucs_model>> models;
    bool isFineTuned = false;
};

namespace tg
{

//==============================================================================
// Helpers
//==============================================================================
namespace
{
    // High-quality-enough SRC for offline use. Lagrange 5-point per channel.
    juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in,
                                       double inRate, double outRate)
    {
        if (juce::approximatelyEqual (inRate, outRate))
            return in;

        const double ratio = inRate / outRate;
        const int outLen = (int) std::ceil (in.getNumSamples() / ratio);
        juce::AudioBuffer<float> out (in.getNumChannels(), outLen);

        for (int ch = 0; ch < in.getNumChannels(); ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, in.getReadPointer (ch),
                            out.getWritePointer (ch), outLen);
        }
        return out;
    }

    Eigen::MatrixXf toEigenStereo (const juce::AudioBuffer<float>& b)
    {
        const int n = b.getNumSamples();
        Eigen::MatrixXf m (2, n);
        const float* L = b.getReadPointer (0);
        const float* R = b.getNumChannels() > 1 ? b.getReadPointer (1) : L;
        for (int i = 0; i < n; ++i) { m (0, i) = L[i]; m (1, i) = R[i]; }
        return m;
    }
} // namespace

//==============================================================================
SeparationEngine::SeparationEngine() : juce::Thread ("Tenganisha Separation")
{
    model = std::make_unique<demucs_model_holder>();
    startThread (juce::Thread::Priority::low); // long-running compute; stay polite
}

SeparationEngine::~SeparationEngine()
{
    signalThreadShouldExit();
    notify();
    stopThread (5000);
}

juce::String SeparationEngine::getStatus() const
{
    const juce::ScopedLock sl (jobLock);
    return statusText;
}

void SeparationEngine::loadModelAsync (const juce::File& ggmlWeights)
{
    if (busy.load()) return;
    {
        const juce::ScopedLock sl (jobLock);
        weightsFile = ggmlWeights;
        statusText  = "Loading model...";
    }
    pendingJob.store ((int) Job::load);
    notify();
}

bool SeparationEngine::separateAsync (juce::AudioBuffer<float> audio,
                                      double hostSampleRate,
                                      juce::int64 timelineStart)
{
    if (busy.load() || ! modelLoaded.load()) return false;
    {
        const juce::ScopedLock sl (jobLock);
        jobAudio         = std::move (audio);
        jobRate          = hostSampleRate;
        jobTimelineStart = timelineStart;
        statusText       = "Separating...";
    }
    pendingJob.store ((int) Job::separate);
    notify();
    return true;
}

void SeparationEngine::cancel()
{
    // demucs.cpp inference is not interruptible mid-segment; we simply drop
    // the result when it lands if a cancel was requested. Kept simple for v1.
    pendingJob.store ((int) Job::none);
}

//==============================================================================
void SeparationEngine::run()
{
    while (! threadShouldExit())
    {
        const auto job = (Job) pendingJob.exchange ((int) Job::none);

        if (job == Job::none)
        {
            wait (200);
            continue;
        }

        busy.store (true);
        progress.store (0.0f);

        if (job == Job::load)
        {
            juce::File f;
            { const juce::ScopedLock sl (jobLock); f = weightsFile; }

            bool ok = false;
            juce::String failText = "Failed to load model";

            // Picking any htdemucs_ft file selects the whole fine-tuned
            // ensemble: its three siblings are found next to it by stem name.
            const bool wantFt = f.getFileName().contains ("htdemucs_ft");
            model->models.clear();
            model->isFineTuned = false;

            if (wantFt)
            {
                ok = f.existsAsFile();
                for (int s = 0; s < kNumStems && ok; ++s)
                {
                    auto matches = f.getParentDirectory().findChildFiles (
                        juce::File::findFiles, false,
                        juce::String ("*htdemucs_ft_") + juce::String (stemName (s)).toLowerCase() + "*.bin");
                    if (matches.isEmpty())
                    {
                        failText = juce::String ("Fine-tuned set incomplete: no ")
                                   + juce::String (stemName (s)).toLowerCase() + " model beside it";
                        ok = false;
                        break;
                    }
                    model->models.push_back (std::make_unique<demucscpp::demucs_model>());
                    ok = demucscpp::load_demucs_model (
                        matches[0].getFullPathName().toStdString(), model->models.back().get());
                }
                model->isFineTuned = ok;
            }
            else if (f.existsAsFile())
            {
                model->models.push_back (std::make_unique<demucscpp::demucs_model>());
                ok = demucscpp::load_demucs_model (f.getFullPathName().toStdString(),
                                                   model->models.back().get());
            }

            if (! ok)
                model->models.clear();

            modelLoaded.store (ok);
            {
                const juce::ScopedLock sl (jobLock);
                statusText = ok ? (model->isFineTuned ? "Model ready (fine-tuned ensemble)"
                                                      : "Model ready")
                                : failText;
            }
            auto cbOk  = onModelLoaded;
            auto cbErr = onError;
            juce::MessageManager::callAsync (
                [alive = std::weak_ptr<int> (callbackLifetime), ok, cbOk, cbErr]
            {
                if (alive.expired()) return; // engine (and its owner) are gone
                if (ok)  { if (cbOk)  cbOk(); }
                else     { if (cbErr) cbErr ("Could not load ggml weights file."); }
            });
        }
        else if (job == Job::separate)
        {
            juce::AudioBuffer<float> audio;
            double rate; juce::int64 tlStart;
            {
                const juce::ScopedLock sl (jobLock);
                audio   = std::move (jobAudio);
                rate    = jobRate;
                tlStart = jobTimelineStart;
            }

            // 1. host rate -> 44.1k (model requirement)
            const double modelRate = (double) demucscpp::SUPPORTED_SAMPLE_RATE;
            auto modelInput = resample (audio, rate, modelRate);
            Eigen::MatrixXf eig = toEigenStereo (modelInput);

            // 2. inference (progress callback ticks our atomic). Fine-tuned
            //    ensemble: run each specialist, keep its own stem's row.
            auto* prog = &progress;
            const int nModels = (int) model->models.size();
            Eigen::Tensor3dXf targets;

            for (int mi = 0; mi < nModels && ! threadShouldExit(); ++mi)
            {
                const float base = (float) mi / (float) nModels;
                auto out = demucscpp::demucs_inference (
                    *model->models[(size_t) mi], eig,
                    [prog, base, nModels] (float p, const std::string&)
                    { prog->store (base + p / (float) nModels); });

                if (mi == 0)
                    targets = std::move (out);
                else // specialist mi owns stem mi
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < (int) out.dimension (2); ++i)
                            targets (mi, ch, i) = out (mi, ch, i);
            }

            if (threadShouldExit())
                break; // shutting down mid-ensemble: don't publish partials

            const int nSources = model->models[0]->is_4sources ? 4 : 6;
            const int nSamples = (int) targets.dimension (2);

            // 3. build StemSet at host rate. 6-source models fold
            //    guitar/piano into "other" to keep a fixed 4-strip UI.
            auto set = std::make_shared<StemSet>();
            set->sampleRate    = rate;
            set->timelineStart = tlStart;

            for (int s = 0; s < kNumStems; ++s)
            {
                juce::AudioBuffer<float> stem441 (2, nSamples);
                for (int ch = 0; ch < 2; ++ch)
                {
                    float* dst = stem441.getWritePointer (ch);
                    for (int i = 0; i < nSamples; ++i)
                        dst[i] = targets ((int) s, ch, i);
                }
                if (nSources == 6 && s == (int) Stem::other)
                    for (int extra = 4; extra < 6; ++extra)
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            float* dst = stem441.getWritePointer (ch);
                            for (int i = 0; i < nSamples; ++i)
                                dst[i] += targets (extra, ch, i);
                        }

                set->stems[(size_t) s] = resample (stem441, modelRate, rate);
            }

            {
                const juce::ScopedLock sl (jobLock);
                statusText = "Done";
            }
            auto cb = onStemsReady;
            StemSetPtr published = set;
            juce::MessageManager::callAsync (
                [alive = std::weak_ptr<int> (callbackLifetime), cb, published]
            {
                if (alive.expired()) return; // engine (and its owner) are gone
                if (cb) cb (published);
            });
        }

        progress.store (1.0f);
        busy.store (false);
    }
}

} // namespace tg
