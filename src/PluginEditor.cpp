#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace tg
{

//==============================================================================
// LookAndFeel
//==============================================================================
TgLookAndFeel::TgLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, palette::window);
    setColour (juce::TextButton::buttonColourId,   palette::panel);
    setColour (juce::TextButton::buttonOnColourId, palette::inset);
    setColour (juce::TextButton::textColourOffId,  palette::text.withAlpha (0.85f));
    setColour (juce::TextButton::textColourOnId,   palette::window);
    setColour (juce::ComboBox::outlineColourId,    palette::hairline);
    setColour (juce::Slider::textBoxTextColourId,     palette::textDim);
    setColour (juce::Slider::textBoxOutlineColourId,  juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, palette::hairline);
    setColour (juce::Label::textColourId, palette::text);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineWhenEditingColourId, palette::hairline);
    setColour (juce::TextEditor::focusedOutlineColourId, palette::textDim.withAlpha (0.5f));
    setColour (juce::CaretComponent::caretColourId, palette::text);
}

juce::Font TgLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return juce::Font (juce::FontOptions (13.0f, juce::Font::plain));
}

void TgLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                          const juce::Colour&, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const float corner = 5.0f;

    auto fill = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId)
                                   : b.findColour (juce::TextButton::buttonColourId);
    if (down)             fill = fill.brighter (0.10f);
    else if (highlighted) fill = fill.brighter (0.05f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, corner);

    g.setColour (b.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, corner, 1.0f);

    if (b.hasKeyboardFocus (true))
    {
        g.setColour (palette::textDim.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (1.5f), corner - 1.0f, 1.0f);
    }
}

void TgLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                      float sliderPos, float, float,
                                      juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearVertical)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, 0, 0, style, s);
        return;
    }

    const auto accent = s.findColour (juce::Slider::trackColourId);
    const float cx = (float) x + (float) w * 0.5f;
    const float top = (float) y + 6.0f, bottom = (float) (y + h) - 6.0f;

    // track
    g.setColour (juce::Colour (0xff34373f));
    g.fillRoundedRectangle (cx - 2.0f, top, 4.0f, bottom - top, 2.0f);

    // filled portion, bottom (-60 dB) up to the thumb
    g.setColour (accent.withAlpha (0.85f));
    g.fillRoundedRectangle (cx - 2.0f, sliderPos, 4.0f, juce::jmax (0.0f, bottom - sliderPos), 2.0f);

    // unity (0 dB) tick
    const auto& range = s.getNormalisableRange();
    const float unityY = bottom - (bottom - top) * (float) range.convertTo0to1 (0.0);
    g.setColour (palette::textDim.withAlpha (0.55f));
    g.fillRect (cx - 7.0f, unityY - 0.5f, 14.0f, 1.0f);

    // thumb: console-fader cap
    const float thumbW = juce::jmin ((float) w - 6.0f, 26.0f), thumbH = 13.0f;
    auto thumb = juce::Rectangle<float> (thumbW, thumbH).withCentre ({ cx, sliderPos });
    g.setColour (juce::Colour (0xff2a2d35));
    g.fillRoundedRectangle (thumb, 3.0f);
    g.setColour (palette::hairline.brighter (0.2f));
    g.drawRoundedRectangle (thumb, 3.0f, 1.0f);
    g.setColour (accent);
    g.fillRect (thumb.reduced (5.0f, 5.5f));
}

//==============================================================================
// StemStrip
//==============================================================================
StemStrip::DragHandle::DragHandle (StemStrip& s) : strip (s)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    setTitle ("Drag stem to DAW");
}

void StemStrip::DragHandle::paint (juce::Graphics& g)
{
    const bool ready = isEnabled();
    const auto c = ready ? palette::textDim : palette::textDim.withAlpha (0.35f);

    // grip dots
    g.setColour (c);
    const float cy = (float) getHeight() * 0.5f;
    float gx = 10.0f;
    for (int i = 0; i < 3; ++i, gx += 5.0f)
        for (int j = -1; j <= 1; j += 2)
            g.fillEllipse (gx, cy + (float) j * 2.5f - 1.0f, 2.0f, 2.0f);

    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    g.drawText (ready ? "drag to DAW" : "no stems yet",
                getLocalBounds().withTrimmedLeft (26).withTrimmedRight (6),
                juce::Justification::centredLeft);
}

void StemStrip::DragHandle::mouseDrag (const juce::MouseEvent& e)
{
    if (isEnabled() && e.getDistanceFromDragStart() > 4)
        strip.startExternalDrag();
}

StemStrip::StemStrip (TenganishaProcessor& p, int stemIndex)
    : proc (p), stem (stemIndex),
      gainAtt (p.apvts, juce::String (stemName (stemIndex)).toLowerCase() + "_gain", gain),
      muteAtt (p.apvts, juce::String (stemName (stemIndex)).toLowerCase() + "_mute", mute),
      soloAtt (p.apvts, juce::String (stemName (stemIndex)).toLowerCase() + "_solo", solo)
{
    const auto accent = palette::stem (stemIndex);

    name.setText (stemName (stemIndex), juce::dontSendNotification);
    name.setJustificationType (juce::Justification::centred);
    name.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    name.setInterceptsMouseClicks (false, false);

    gain.setColour (juce::Slider::trackColourId, accent);
    gain.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    gain.setColour (juce::Slider::textBoxTextColourId, palette::textDim);
    gain.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 18);
    gain.setTextValueSuffix (" dB");

    for (auto* b : { &mute, &solo })
    {
        b->setClickingTogglesState (true);
        b->setColour (juce::TextButton::textColourOnId, palette::window);
    }
    mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffc25a5a));
    solo.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd9b968));
    mute.setTitle (juce::String (stemName (stemIndex)) + " mute");
    solo.setTitle (juce::String (stemName (stemIndex)) + " solo");

    addAndMakeVisible (name);
    addAndMakeVisible (gain);
    addAndMakeVisible (mute);
    addAndMakeVisible (solo);
    addAndMakeVisible (handle);
    handle.setEnabled (false);
}

void StemStrip::setStemsAvailable (bool available)
{
    if (stemsAvailable == available) return;
    stemsAvailable = available;
    handle.setEnabled (available);
    handle.repaint();
}

void StemStrip::resized()
{
    auto r = getLocalBounds().reduced (6);
    r.removeFromTop (4); // below the colour tab
    name.setBounds (r.removeFromTop (20));
    handle.setBounds (r.removeFromBottom (22));
    auto btns = r.removeFromBottom (26).reduced (2, 2);
    mute.setBounds (btns.removeFromLeft (btns.getWidth() / 2).reduced (2, 0));
    solo.setBounds (btns.reduced (2, 0));
    gain.setBounds (r.reduced (0, 2));
}

void StemStrip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (palette::panel);
    g.fillRoundedRectangle (r, 7.0f);
    g.setColour (palette::hairline);
    g.drawRoundedRectangle (r, 7.0f, 1.0f);

    // stem colour tab
    auto tab = getLocalBounds().toFloat().reduced (7.0f, 0).removeFromTop (8.0f).withTrimmedTop (5.0f);
    g.setColour (palette::stem (stem).withAlpha (stemsAvailable ? 1.0f : 0.45f));
    g.fillRoundedRectangle (tab, 1.5f);
}

juce::File StemStrip::renderStemToTempWav() const
{
    auto stems = proc.getStems();
    if (stems == nullptr) return {};

    auto dir  = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("Tenganisha");
    dir.createDirectory();
    auto file = dir.getChildFile ("tenganisha-" + juce::String (stemName (stem)).toLowerCase() + ".wav");
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

void StemStrip::startExternalDrag()
{
    if (isDragAndDropActive()) return;
    auto f = renderStemToTempWav();
    if (f.existsAsFile())
        performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
}

//==============================================================================
// Editor
//==============================================================================
TenganishaEditor::TenganishaEditor (TenganishaProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&lookAndFeel);

    for (int s = 0; s < kNumStems; ++s)
        addAndMakeVisible (strips.add (new StemStrip (proc, s)));

    addAndMakeVisible (recordBtn);
    addAndMakeVisible (separateBtn);
    addAndMakeVisible (discardBtn);
    addAndMakeVisible (loadModelBtn);

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

    setResizable (true, true);
    setResizeLimits (560, 400, 1000, 720);
    setSize (600, 430); // last: fires a synchronous resized()
}

TenganishaEditor::~TenganishaEditor()
{
    setLookAndFeel (nullptr);
}

void TenganishaEditor::timerCallback()
{
    const auto st = proc.getEngineState();
    const auto progress = proc.engine.getProgress();

    updateForState();

    // Repaint only when something visible moves; recording animates the pulse.
    if (st != lastState
        || st == EngineState::recording
        || (st == EngineState::separating && ! juce::approximatelyEqual (progress, displayedProgress)))
    {
        displayedProgress = progress;
        lastState = st;
        repaint();
    }
}

void TenganishaEditor::updateForState()
{
    const auto st = proc.getEngineState();
    const bool modelReady = proc.engine.isModelLoaded();

    recordBtn.setEnabled   (modelReady && st == EngineState::passthrough);
    separateBtn.setEnabled (st == EngineState::recording);
    discardBtn.setEnabled  (st == EngineState::stemPlayback);
    loadModelBtn.setEnabled (! proc.engine.isBusy());

    // The expected next action carries an accent; everything else stays quiet.
    recordBtn.setColour (juce::TextButton::textColourOffId,
                         recordBtn.isEnabled() ? palette::record : palette::text.withAlpha (0.85f));
    separateBtn.setColour (juce::TextButton::textColourOffId,
                           separateBtn.isEnabled() ? palette::stem (3) : palette::text.withAlpha (0.85f));
    loadModelBtn.setColour (juce::TextButton::textColourOffId,
                            modelReady ? palette::textDim : palette::text.withAlpha (0.85f));

    juce::String s;
    switch (st)
    {
        case EngineState::passthrough:
            s = modelReady ? "Press Record, play the section in your DAW, then Separate"
                           : "Load an HTDemucs model (.bin) to begin";
            statePill = modelReady ? "READY" : "NO MODEL";
            statePillColour = palette::textDim;
            break;
        case EngineState::recording:
            s = "Recording " + juce::String (proc.getRecordedSeconds(), 1) + " s"
                + (proc.getRecordedSeconds() < 0.05 ? "  (press play in your DAW)" : "");
            statePill = "RECORDING";
            statePillColour = palette::record;
            break;
        case EngineState::separating:
            s = "Separating " + juce::String ((int) (displayedProgress * 100)) + "%";
            statePill = "SEPARATING";
            statePillColour = palette::stem (3);
            break;
        case EngineState::stemPlayback:
            s = "Stems live on the timeline. Mix inline or drag a strip into your DAW";
            statePill = "STEMS LIVE";
            statePillColour = palette::stem (2);
            break;
    }

    if (s != statusText)
    {
        statusText = s;
        repaint (statusArea);
        repaint (headerArea);
    }

    const bool haveStems = proc.getStems() != nullptr;
    for (auto* strip : strips)
        strip->setStemsAvailable (haveStems);
}

//==============================================================================
void TenganishaEditor::drawHeader (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area;

    // Signature: one signal line fanning into the four stem strands.
    auto glyph = r.removeFromLeft (58).toFloat();
    const float cy = glyph.getCentreY();
    const float x0 = glyph.getX(), x1 = glyph.getRight() - 8.0f;
    const float mid = x0 + (x1 - x0) * 0.42f;

    g.setColour (palette::text.withAlpha (0.8f));
    g.drawLine (x0, cy, mid, cy, 1.6f);

    for (int s = 0; s < kNumStems; ++s)
    {
        const float yEnd = cy + ((float) s - 1.5f) * 5.5f;
        juce::Path strand;
        strand.startNewSubPath (mid, cy);
        strand.cubicTo (mid + 9.0f, cy, x1 - 12.0f, yEnd, x1, yEnd);
        g.setColour (palette::stem (s));
        g.strokePath (strand, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    g.setColour (palette::text);
    g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold))
                   .withExtraKerningFactor (0.16f));
    g.drawText ("TENGANISHA", r.reduced (2, 0), juce::Justification::centredLeft);

    // state pill, top right
    g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold))
                   .withExtraKerningFactor (0.08f));
    juce::GlyphArrangement ga;
    ga.addLineOfText (g.getCurrentFont(), statePill, 0.0f, 0.0f);
    const int pillW = juce::jmax (64, 18 + (int) ga.getBoundingBox (0, -1, true).getWidth());
    auto pill = r.removeFromRight (pillW).withSizeKeepingCentre (pillW, 20).toFloat();
    g.setColour (statePillColour.withAlpha (0.16f));
    g.fillRoundedRectangle (pill, 10.0f);
    g.setColour (statePillColour);
    g.drawText (statePill, pill.toNearestInt(), juce::Justification::centred);
}

void TenganishaEditor::drawStatusRow (juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area;
    const auto st = proc.getEngineState();

    if (st == EngineState::recording)
    {
        const float pulse = 0.55f + 0.45f * std::sin ((float) juce::Time::getMillisecondCounter() * 0.008f);
        auto dot = r.removeFromLeft (16).toFloat().withSizeKeepingCentre (8.0f, 8.0f);
        g.setColour (palette::record.withAlpha (pulse));
        g.fillEllipse (dot);
    }

    g.setColour (palette::textDim);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText (statusText, r.withTrimmedBottom (6), juce::Justification::centredLeft);

    if (st == EngineState::separating)
    {
        // Progress as the four strands sweeping together.
        auto bar = area.removeFromBottom (4).toFloat().reduced (0.0f, 0.5f);
        g.setColour (palette::hairline);
        g.fillRoundedRectangle (bar, 1.5f);
        auto done = bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, displayedProgress));
        const float quarter = done.getWidth() / 4.0f;
        for (int s = 0; s < kNumStems; ++s)
        {
            g.setColour (palette::stem (s));
            g.fillRect (done.getX() + quarter * (float) s, done.getY(), quarter, done.getHeight());
        }
    }
}

void TenganishaEditor::paint (juce::Graphics& g)
{
    g.fillAll (palette::window);
    drawHeader (g, headerArea);

    g.setColour (palette::hairline);
    g.fillRect (headerArea.getX(), headerArea.getBottom() + 2, headerArea.getWidth(), 1);

    drawStatusRow (g, statusArea);
}

void TenganishaEditor::resized()
{
    auto r = getLocalBounds().reduced (14);

    headerArea = r.removeFromTop (34);
    r.removeFromTop (8);

    auto transport = r.removeFromTop (30);
    recordBtn.setBounds   (transport.removeFromLeft (88));
    transport.removeFromLeft (6);
    separateBtn.setBounds (transport.removeFromLeft (88));
    transport.removeFromLeft (6);
    discardBtn.setBounds  (transport.removeFromLeft (88));
    loadModelBtn.setBounds (transport.removeFromRight (108));

    r.removeFromTop (4);
    statusArea = r.removeFromTop (26);
    r.removeFromTop (6);

    const int gap = 10;
    const int w = (r.getWidth() - gap * (kNumStems - 1)) / kNumStems;
    for (int s = 0; s < juce::jmin ((int) strips.size(), kNumStems); ++s)
    {
        strips[s]->setBounds (r.removeFromLeft (w));
        if (s < kNumStems - 1) r.removeFromLeft (gap);
    }
}

} // namespace tg
