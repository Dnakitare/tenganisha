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

    for (int s = 0; s < kNumStems; ++s)
    {
        auto id = juce::String (stemName (s)).toLowerCase();
        layout.add (std::make_unique<P> (juce::ParameterID { id + "_gain", 1 },
                                         juce::String (stemName (s)) + " Gain",
                                         juce::NormalisableRange<float> (-60.0f, 12.0f, 0.1f),
                                         0.0f));
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
    hostRate = sampleRate;
    const int maxSamples = (int) (kMaxCaptureSeconds * sampleRate);
    captureBuffer.setSize (2, maxSamples, false, false, true);
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

    engine.separateAsync (std::move (copy), hostRate, captureTimelineStart);
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
            const int written = captureLength.load();
            const int room    = captureBuffer.getNumSamples() - written;
            const int n       = juce::jmin (numSamples, room);
            if (written == 0)
                captureTimelineStart = playPos;
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

            for (int s = 0; s < kNumStems; ++s)
            {
                const bool muted = muteParams[(size_t) s]->load() > 0.5f;
                const bool solod = soloParams[(size_t) s]->load() > 0.5f;
                const bool audible = ! muted && (! anySolo || solod);

                float target = audible
                    ? juce::Decibels::decibelsToGain (gainParams[(size_t) s]->load()) * master
                    : 0.0f;
                smoothedGain[(size_t) s].setTargetValue (target);

                const auto& stem = stems->stems[(size_t) s];
                for (int i = 0; i < numSamples; ++i)
                {
                    const juce::int64 idx = offset + i;
                    const float g = smoothedGain[(size_t) s].getNextValue();
                    if (idx < 0 || idx >= stem.getNumSamples()) continue;
                    for (int ch = 0; ch < 2; ++ch)
                        buffer.addSample (ch, i, stem.getSample (ch, (int) idx) * g);
                }
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
