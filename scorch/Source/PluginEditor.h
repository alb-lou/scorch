#pragma once
#include "PluginProcessor.h"

class ScorchEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ScorchEditor (ScorchProcessor&);
    ~ScorchEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    ScorchProcessor& proc;

    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    std::array<Knob, 7> knobs;
    std::array<juce::TextButton, 5> styleButtons;
    juce::TextButton punish { "Punish" }, steep { "Steep" }, autoGain { "Auto Gain" }, calibrate { "Calibrate" }, reset { "Reset" };
    juce::Label status;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> aPunish, aSteep, aAuto, aCal;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScorchEditor)
};
