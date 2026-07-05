#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace tg
{

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout TenganishaProcessor::makeLayout()
{
    using P = juce::AudioParameterFloat;
    using B = juce::AudioParameterBool;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Console-style fader taper: -12 dB at mid travel, fine resolution near unity.
    auto gainRange = juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f);
    gainRange.setSkewForCentre (-12.0f);

    for (int s = 0; s < kNumStems; ++s)
    {
        auto id = juce::String (stemName (s)).toLowerCase();
        layout.add (std::make_unique<P> (juce::ParameterID { id + "_gain", 1 },
                                         juce::String (stemName (s)) + " Gain",
                                         gainRange, 0.0f));
        layout.add (std::make_unique<B> (juce::ParameterID { id + "_mute", 1 },
                                         juce::String (stemName (s)) + " Mute", false));
        layout.add (std::make_unique<B> (juce::ParameterID { id + "_solo", 1 },
                                         juce::String (stemName (s)) + " Solo", false));
    }
    layout.add (std::make_unique<P> (juce::ParameterID { "master", 1 }, "Master",
                                     juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f),
                                     0.0f));
    return layout;
}

TenganishaProcessor::TenganishaProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", makeLayout())
{
    for (int s = 0; s < kNumStems; ++s)
    {
        auto id = juce::String (stemName (s)).toLowerCase();
        gainParams[(size_t) s] = apvts.getRawParameterValue (id + "_gain");
        muteParams[(size_t) s] = apvts.getRawParameterValue (id + "_mute");
        soloParams[(size_t) s] = apvts.getRawParameterValue (id + "_solo");
    }
    masterParam = apvts.getRawParameterValue ("master");

    engine.onStemsReady = [this] (StemSetPtr set)
    {
        std::atomic_store (&currentStems, set);
        engineState.store ((int) EngineState::stemPlayback);
    };
    engine.onError = [this] (juce::String)
    {
        engineState.store ((int) EngineState::passthrough);
    };
}

TenganishaProcessor::~TenganishaProcessor() = default;

//==============================================================================
void TenganishaProcessor::prepareToPlay (double sampleRate, int)
{
    // Stems are indexed at the rate they were built for; playing them at a
    // different rate would be mispitched and misaligned. Drop them.
    if (auto stems = std::atomic_load (&currentStems))
        if (! juce::approximatelyEqual (stems->sampleRate, sampleRate))
        {
            std::atomic_store (&currentStems, StemSetPtr());
            engineState.store ((int) EngineState::passthrough);
        }

    hostRate = sampleRate;
    for (auto& g : smoothedGain)
        g.reset (sampleRate, 0.02);
}

bool TenganishaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
}

double TenganishaProcessor::getRecordedSeconds() const
{
    return captureLength.load() / hostRate;
}

//==============================================================================
void TenganishaProcessor::startRecording()
{
    if (getEngineState() == EngineState::recording) return;

    // Allocate here (message thread), not per idle instance in prepareToPlay.
    // Must complete before the state flips to recording.
    const int maxSamples = (int) (kMaxCaptureSeconds * hostRate);
    if (captureBuffer.getNumSamples() < maxSamples)
        captureBuffer.setSize (2, maxSamples, false, false, true);

    captureLength.store (0);
    internalPos = 0;
    engineState.store ((int) EngineState::recording);
}

void TenganishaProcessor::stopRecordingAndSeparate()
{
    if (getEngineState() != EngineState::recording) return;
    engineState.store ((int) EngineState::separating);

    const int len = captureLength.load();
    if (len < (int) hostRate / 4) // < 250 ms: nothing useful
    {
        engineState.store ((int) EngineState::passthrough);
        return;
    }

    juce::AudioBuffer<float> copy (2, len);
    for (int ch = 0; ch < 2; ++ch)
        copy.copyFrom (ch, 0, captureBuffer, ch, 0, len);

    // If the engine can't take the job (still loading a model the user picked
    // mid-take, or another job in flight), onStemsReady will never fire —
    // fall back to passthrough instead of waiting in `separating` forever.
    if (! engine.separateAsync (std::move (copy), hostRate, captureTimelineStart.load()))
        engineState.store ((int) EngineState::passthrough);
}

void TenganishaProcessor::discardStems()
{
    std::atomic_store (&currentStems, StemSetPtr());
    engineState.store ((int) EngineState::passthrough);
}

//==============================================================================
void TenganishaProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const auto state = (EngineState) engineState.load();

    // Host playhead position (falls back to a free-running counter)
    juce::int64 playPos = internalPos;
    bool hostPlaying = true;
    if (auto* ph = getPlayHead())
        if (auto info = ph->getPosition())
        {
            if (auto t = info->getTimeInSamples()) playPos = *t;
            hostPlaying = info->getIsPlaying();
        }

    switch (state)
    {
        case EngineState::recording:
        {
            // Hosts keep running FX while the transport is stopped (REAPER's
            // anticipative processing even runs faster than realtime). Only
            // capture timeline-attached audio, so the first *playing* block
            // stamps captureTimelineStart and stopped silence never lands in
            // the buffer.
            if (! hostPlaying)
                break;

            const int written = captureLength.load();
            const int room    = captureBuffer.getNumSamples() - written;
            const int n       = juce::jmin (numSamples, room);
            if (written == 0)
                captureTimelineStart.store (playPos);
            if (n > 0)
            {
                for (int ch = 0; ch < 2; ++ch)
                    captureBuffer.copyFrom (ch, written, buffer,
                                            juce::jmin (ch, buffer.getNumChannels() - 1), 0, n);
                captureLength.store (written + n);
            }
            // passthrough while recording — leave buffer untouched
            break;
        }

        case EngineState::stemPlayback:
        {
            auto stems = std::atomic_load (&currentStems);
            if (stems == nullptr) { break; }

            // solo logic: if any solo is active, only soloed stems sound
            bool anySolo = false;
            for (int s = 0; s < kNumStems; ++s)
                anySolo |= (soloParams[(size_t) s]->load() > 0.5f);

            const float master = juce::Decibels::decibelsToGain (masterParam->load());

            buffer.clear();
            const juce::int64 offset = playPos - stems->timelineStart;

            // Clamp the copy range once per block instead of per sample.
            const juce::int64 srcStart = juce::jmax<juce::int64> (offset, 0);
            const juce::int64 srcEnd   = juce::jmin<juce::int64> (offset + numSamples,
                                                                  stems->numSamples());
            const int n = (int) juce::jmax<juce::int64> (0, srcEnd - srcStart);
            const int dstStart = n > 0 ? (int) (srcStart - offset) : 0;

            for (int s = 0; s < kNumStems; ++s)
            {
                const bool muted = muteParams[(size_t) s]->load() > 0.5f;
                const bool solod = soloParams[(size_t) s]->load() > 0.5f;
                const bool audible = ! muted && (! anySolo || solod);

                const float target = audible
                    ? juce::Decibels::decibelsToGain (gainParams[(size_t) s]->load()) * master
                    : 0.0f;

                auto& smooth = smoothedGain[(size_t) s];
                smooth.setTargetValue (target);
                const float g0 = smooth.getCurrentValue();
                const float g1 = smooth.skip (numSamples); // advance one block

                if (n <= 0 || (g0 < 1.0e-6f && g1 < 1.0e-6f))
                    continue;

                // Gain endpoints for the in-range slice of the block ramp.
                const float t0 = (float) dstStart / (float) numSamples;
                const float t1 = (float) (dstStart + n) / (float) numSamples;
                const float ga = g0 + (g1 - g0) * t0;
                const float gb = g0 + (g1 - g0) * t1;

                const auto& stem = stems->stems[(size_t) s];
                for (int ch = 0; ch < 2; ++ch)
                    buffer.addFromWithRamp (ch, dstStart,
                                            stem.getReadPointer (ch, (int) srcStart),
                                            n, ga, gb);
            }
            break;
        }

        case EngineState::passthrough:
        case EngineState::separating:
        default:
            break; // input -> output untouched
    }

    if (hostPlaying)
        internalPos += numSamples;
}

//==============================================================================
void TenganishaProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void TenganishaProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* TenganishaProcessor::createEditor()
{
    return new TenganishaEditor (*this);
}

} // namespace tg

// JUCE entry point
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tg::TenganishaProcessor();
}
