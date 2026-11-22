#include "PluginEditor.h"

using namespace juce;

static const char* kKnobIds[7]   = { "drive", "tone", "lowcut", "thump", "highcut", "mix", "output" };
static const char* kKnobNames[7] = { "Drive", "Tone", "Low Cut", "Thump", "High Cut", "Mix", "Output" };
static const char* kStyleNames[5] = { "Tape Tube", "Console", "Germanium", "Triode", "Pentode" };

ScorchEditor::ScorchEditor (ScorchProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setSize (700, 380);

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        k.slider.setColour (Slider::rotarySliderFillColourId, Colour (0xffff7a2f));
        k.slider.setColour (Slider::thumbColourId, Colours::white);
        k.slider.setColour (Slider::textBoxTextColourId, Colours::white);
        k.slider.setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
        k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 80, 20);
        if (i == 2) k.slider.textFromValueFunction = [] (double v) { return v <= 20.5 ? String ("Off") : String ((int) v) + " Hz"; };
        if (i == 4) k.slider.textFromValueFunction = [] (double v) { return v >= 19990 ? String ("Off") : (v >= 1000 ? String (v / 1000.0, 1) + " kHz" : String ((int) v) + " Hz"); };
        if (i == 5) k.slider.setTextValueSuffix (" %");
        if (i == 6) k.slider.setTextValueSuffix (" dB");
        addAndMakeVisible (k.slider);
        k.label.setText (kKnobNames[i], dontSendNotification);
        k.label.setJustificationType (Justification::centred);
        k.label.setColour (Label::textColourId, Colours::lightgrey);
        addAndMakeVisible (k.label);
        k.att = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, kKnobIds[i], k.slider);
    }

    for (size_t i = 0; i < styleButtons.size(); ++i)
    {
        auto& b = styleButtons[i];
        b.setButtonText (kStyleNames[i]);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1);
        b.setColour (TextButton::buttonOnColourId, Colour (0xffff7a2f));
        b.onClick = [this, i]
        {
            if (auto* c = dynamic_cast<AudioParameterChoice*> (proc.apvts.getParameter ("style")))
            { c->beginChangeGesture(); *c = (int) i; c->endChangeGesture(); }
        };
        addAndMakeVisible (b);
    }

    for (auto* b : { &punish, &steep, &autoGain, &calibrate })
    {
        b->setClickingTogglesState (true);
        b->setColour (TextButton::buttonOnColourId, Colour (0xffff7a2f));
        addAndMakeVisible (b);
    }
    addAndMakeVisible (reset);
    reset.onClick = [this] { proc.resetCalibration(); };
    aPunish = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "punish", punish);
    aSteep  = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "steep", steep);
    aAuto   = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "autogain", autoGain);
    aCal    = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "calibrate", calibrate);

    status.setJustificationType (Justification::centredLeft);
    status.setColour (Label::textColourId, Colours::lightgrey);
    addAndMakeVisible (status);
    startTimerHz (15);
    timerCallback();
}

ScorchEditor::~ScorchEditor() { stopTimer(); }

void ScorchEditor::timerCallback()
{
    const int s = (int) *proc.apvts.getRawParameterValue ("style");
    for (size_t i = 0; i < styleButtons.size(); ++i)
        styleButtons[i].setToggleState ((int) i == s, dontSendNotification);

    String t;
    if (proc.calibrating.load())          t = "Calibrating... play your loudest section, then press Calibrate again";
    else switch (proc.compSource.load())
    {
        case 1:  t = "Auto Gain: " + String (proc.displayCompDb.load(), 1) + " dB (built-in curve)"; break;
        case 2:  t = "Auto Gain: " + String (proc.displayCompDb.load(), 1) + " dB (calibrated)"; break;
        default: t = "Auto Gain off"; break;
    }
    status.setText (t, dontSendNotification);
    reset.setEnabled (proc.calValid.load());
}

void ScorchEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1b1b1f));
    g.setColour (Colour (0xffff7a2f));
    g.setFont (FontOptions (26.0f, Font::bold));
    g.drawText ("SCORCH", 20, 10, 200, 34, Justification::centredLeft);
    g.setColour (Colours::grey);
    g.setFont (FontOptions (13.0f));
    g.drawText ("saturation", 20, 38, 200, 18, Justification::centredLeft);
}

void ScorchEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (50);
    auto styleRow = area.removeFromTop (34);
    const int sw = styleRow.getWidth() / (int) styleButtons.size();
    for (auto& b : styleButtons) b.setBounds (styleRow.removeFromLeft (sw).reduced (4, 0));

    area.removeFromTop (14);
    auto knobRow = area.removeFromTop (170);
    const int kw = knobRow.getWidth() / (int) knobs.size();
    for (auto& k : knobs)
    {
        auto cell = knobRow.removeFromLeft (kw);
        k.label.setBounds (cell.removeFromTop (20));
        k.slider.setBounds (cell);
    }

    area.removeFromTop (12);
    auto row = area.removeFromTop (34);
    for (auto* b : { &punish, &steep, &autoGain, &calibrate, &reset })
        b->setBounds (row.removeFromLeft (110).reduced (4, 0));
    area.removeFromTop (8);
    status.setBounds (area.removeFromTop (24));
}
