#pragma once
#include "PluginProcessor.h"

namespace tg
{

//==============================================================================
// One channel strip per stem: name, gain slider, mute/solo, drag-to-DAW export.
class StemStrip : public juce::Component,
                  public juce::DragAndDropContainer
{
public:
    StemStrip (TenganishaProcessor& p, int stemIndex);
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    juce::File renderStemToTempWav() const;

    TenganishaProcessor& proc;
    const int stem;

    juce::Slider gain { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Label name;

    juce::AudioProcessorValueTreeState::SliderAttachment gainAtt;
    juce::AudioProcessorValueTreeState::ButtonAttachment muteAtt, soloAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemStrip)
};

//==============================================================================
class TenganishaEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit TenganishaEditor (TenganishaProcessor&);
    ~TenganishaEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateForState();

    TenganishaProcessor& proc;

    juce::TextButton loadModelBtn { "Load Model..." };
    juce::TextButton recordBtn    { "Record" };
    juce::TextButton separateBtn  { "Separate" };
    juce::TextButton discardBtn   { "Discard" };
    juce::Label      statusLabel;
    double           displayedProgress = 0.0;
    juce::ProgressBar progressBar { displayedProgress };

    juce::OwnedArray<StemStrip> strips;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TenganishaEditor)
};

} // namespace tg
