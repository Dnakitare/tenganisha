#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace tg
{

//==============================================================================
StemStrip::StemStrip (TenganishaProcessor& p, int stemIndex)
    : proc (p), stem (stemIndex),
      gainAtt (p.apvts, juce::String (stemName (stemIndex)).toLowerCase() + "_gain", gain),
      muteAtt (p.apvts, juce::String (stemName (stemIndex)).toLowerCase() + "_mute", mute),
      soloAtt (p.apvts, juce::String (stemName (stemIndex)).toLowerCase() + "_solo", solo)
{
    name.setText (stemName (stemIndex), juce::dontSendNotification);
    name.setJustificationType (juce::Justification::centred);
    mute.setClickingTogglesState (true);
    solo.setClickingTogglesState (true);
    mute.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    solo.setColour (juce::TextButton::buttonOnColourId, juce::Colours::gold);

    addAndMakeVisible (name);
    addAndMakeVisible (gain);
    addAndMakeVisible (mute);
    addAndMakeVisible (solo);
}

void StemStrip::resized()
{
    auto r = getLocalBounds().reduced (4);
    name.setBounds (r.removeFromTop (20));
    auto btns = r.removeFromBottom (26);
    mute.setBounds (btns.removeFromLeft (btns.getWidth() / 2).reduced (2));
    solo.setBounds (btns.reduced (2));
    gain.setBounds (r);
}

void StemStrip::paint (juce::Graphics& g)
{
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f);
    if (proc.getStems() != nullptr)
    {
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (11.0f);
        g.drawText ("drag to DAW", getLocalBounds().removeFromTop (36).removeFromBottom (14),
                    juce::Justification::centred);
    }
}

juce::File StemStrip::renderStemToTempWav() const
{
    auto stems = proc.getStems();
    if (stems == nullptr) return {};

    auto dir  = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("Tenganisha");
    dir.createDirectory();
    auto file = dir.getChildFile (juce::String (stemName (stem)).toLowerCase() + ".wav");
    file.deleteFile();

    juce::WavAudioFormat fmt;
    if (auto stream = file.createOutputStream())
    {
        std::unique_ptr<juce::AudioFormatWriter> writer (
            fmt.createWriterFor (stream.get(), stems->sampleRate, 2, 24, {}, 0));
        if (writer != nullptr)
        {
            stream.release(); // writer owns it now
            writer->writeFromAudioSampleBuffer (stems->stems[(size_t) stem], 0,
                                                stems->numSamples());
        }
    }
    return file;
}

void StemStrip::mouseDrag (const juce::MouseEvent&)
{
    if (isDragAndDropActive()) return;
    auto f = renderStemToTempWav();
    if (f.existsAsFile())
        performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
}

//==============================================================================
TenganishaEditor::TenganishaEditor (TenganishaProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    setSize (520, 380);

    for (int s = 0; s < kNumStems; ++s)
        addAndMakeVisible (strips.add (new StemStrip (proc, s)));

    addAndMakeVisible (loadModelBtn);
    addAndMakeVisible (recordBtn);
    addAndMakeVisible (separateBtn);
    addAndMakeVisible (discardBtn);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (progressBar);

    statusLabel.setJustificationType (juce::Justification::centredLeft);

    loadModelBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (
            "Select ggml model weights (.bin)",
            juce::File::getSpecialLocation (juce::File::userHomeDirectory),
            "*.bin");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                if (fc.getResult().existsAsFile())
                    proc.engine.loadModelAsync (fc.getResult());
            });
    };

    recordBtn.onClick   = [this] { proc.startRecording(); };
    separateBtn.onClick = [this] { proc.stopRecordingAndSeparate(); };
    discardBtn.onClick  = [this] { proc.discardStems(); };

    startTimerHz (15);
    updateForState();
}

void TenganishaEditor::timerCallback()
{
    displayedProgress = proc.engine.getProgress();
    updateForState();
    repaint();
}

void TenganishaEditor::updateForState()
{
    const auto st = proc.getEngineState();
    const bool modelReady = proc.engine.isModelLoaded();

    recordBtn.setEnabled   (modelReady && st == EngineState::passthrough);
    separateBtn.setEnabled (st == EngineState::recording);
    discardBtn.setEnabled  (st == EngineState::stemPlayback);
    loadModelBtn.setEnabled (! proc.engine.isBusy());

    juce::String s;
    switch (st)
    {
        case EngineState::passthrough:
            s = modelReady ? "Ready — press Record, play the section, then Separate"
                           : "Load a ggml HTDemucs model to begin";
            break;
        case EngineState::recording:
            s = "Recording... " + juce::String (proc.getRecordedSeconds(), 1) + " s";
            break;
        case EngineState::separating:
            s = "Separating... " + juce::String ((int) (displayedProgress * 100)) + "%";
            break;
        case EngineState::stemPlayback:
            s = "Stems live — mixing replaces input on the timeline";
            break;
    }
    statusLabel.setText (s, juce::dontSendNotification);
    progressBar.setVisible (st == EngineState::separating);
}

void TenganishaEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff181820));
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    g.drawText ("Tenganisha", 12, 8, 220, 24, juce::Justification::centredLeft);
}

void TenganishaEditor::resized()
{
    auto r = getLocalBounds().reduced (10);
    r.removeFromTop (28); // title

    auto top = r.removeFromTop (32);
    loadModelBtn.setBounds (top.removeFromLeft (110).reduced (2));
    recordBtn.setBounds    (top.removeFromLeft (90).reduced (2));
    separateBtn.setBounds  (top.removeFromLeft (90).reduced (2));
    discardBtn.setBounds   (top.removeFromLeft (90).reduced (2));

    auto status = r.removeFromTop (26);
    statusLabel.setBounds (status.removeFromLeft (status.getWidth() - 140));
    progressBar.setBounds (status);

    const int w = r.getWidth() / kNumStems;
    for (int s = 0; s < kNumStems; ++s)
        strips[s]->setBounds (r.removeFromLeft (w).reduced (4));
}

} // namespace tg
