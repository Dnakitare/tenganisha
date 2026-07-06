#pragma once
#include "PluginProcessor.h"

namespace tg
{

//==============================================================================
// Palette: warm graphite console, four stem hues that read as one set.
namespace palette
{
    inline const juce::Colour window   { 0xff17181c };
    inline const juce::Colour panel    { 0xff1e2026 };
    inline const juce::Colour inset    { 0xff23252d };
    inline const juce::Colour hairline { 0xff2c2f37 };
    inline const juce::Colour text     { 0xffe8e6e1 };
    inline const juce::Colour textDim  { 0xff8f929c };
    inline const juce::Colour record   { 0xffe5484d };

    inline juce::Colour stem (int i)
    {
        static const juce::Colour c[] = { juce::Colour (0xffe5604c),   // drums: clay
                                          juce::Colour (0xff5b7fde),   // bass: cobalt
                                          juce::Colour (0xff45b69c),   // other: verdigris
                                          juce::Colour (0xffe0a93e) }; // vocals: brass
        return c[juce::jlimit (0, 3, i)];
    }
}

//==============================================================================
class TgLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TgLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
};

//==============================================================================
// One channel strip per stem: colour tab, gain fader, mute/solo, drag handle.
class StemStrip : public juce::Component,
                  public juce::DragAndDropContainer
{
public:
    StemStrip (TenganishaProcessor& p, int stemIndex);
    void resized() override;
    void paint (juce::Graphics&) override;

    void startExternalDrag();
    void setStemsAvailable (bool available);

private:
    // Dedicated grab area: the fader owns most of the strip surface, so the
    // export drag needs its own component (and cursor affordance).
    class DragHandle : public juce::Component
    {
    public:
        explicit DragHandle (StemStrip& s);
        void paint (juce::Graphics&) override;
        void mouseDrag (const juce::MouseEvent&) override;
    private:
        StemStrip& strip;
    };

    juce::File renderStemToTempWav() const;

    TenganishaProcessor& proc;
    const int stem;
    bool stemsAvailable = false;

    juce::Label      name;
    juce::Slider     gain { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::TextButton mute { "M" }, solo { "S" };
    DragHandle       handle { *this };

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
    ~TenganishaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateForState();
    void drawHeader (juce::Graphics&, juce::Rectangle<int> area);
    void drawStatusRow (juce::Graphics&, juce::Rectangle<int> area);

    TenganishaProcessor& proc;
    TgLookAndFeel lookAndFeel;

    void onModelChoice();
    void startDownload (int kind);
    void pollDownload();
    void selectComboForPath (const juce::String& path);

    juce::TextButton recordBtn    { "Record" };
    juce::TextButton separateBtn  { "Separate" };
    juce::TextButton discardBtn   { "Discard" };
    juce::ComboBox   modelBox;

    juce::OwnedArray<StemStrip> strips;
    std::unique_ptr<juce::FileChooser> chooser;

    // in-plugin model download (sequential files, e.g. the ft ensemble)
    std::unique_ptr<juce::URL::DownloadTask> dlTask;
    juce::StringArray dlRemaining;   // filenames still to fetch
    int   dlKind       = -1;         // models::Kind while downloading, else -1
    int   dlTotalFiles = 0;
    float dlProgress   = 0.0f;
    juce::String downloadError;

    juce::Rectangle<int> headerArea, statusArea;
    juce::String statusText, statePill;
    juce::Colour statePillColour;
    float displayedProgress = 0.0f;
    EngineState lastState = EngineState::passthrough;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TenganishaEditor)
};

} // namespace tg
