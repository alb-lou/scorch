#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include "Dsp.h"

class ScorchProcessor : public juce::AudioProcessor
{
public:
    ScorchProcessor();
    ~ScorchProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processChunk (juce::AudioBuffer<float>&);

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Scorch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    juce::AudioProcessorValueTreeState apvts;

    void resetCalibration() { calValid.store (false); }

    // read by the editor
    std::atomic<float> displayCompDb { 0.0f };
    std::atomic<int>   compSource { 0 };          // 0 = off, 1 = built-in curve, 2 = calibrated
    std::atomic<bool>  calibrating { false };
    std::atomic<bool>  calValid { false };

private:
    static constexpr int kNumSig = 8;
    using Sig = std::array<float, kNumSig>;
    bool sigMatches (const Sig&) const;

    std::atomic<float> *pStyle, *pDrive, *pPunish, *pTone, *pLowCut, *pThump, *pHighCut, *pSteep, *pMix, *pOutput, *pAuto, *pCal;

    std::array<sat::StyleCore, 2> cores;
    std::array<sat::Biquad, 2> lowCut, thump, toneLo, toneHi, hc1, hc2;
    int lastStyle = -1;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 8192 };
    juce::AudioBuffer<float> dryBuf;
    std::vector<float> gDrive, gComp, gMix, gOut;
    juce::SmoothedValue<float> driveSm, compSm, mixSm, outSm;
    double fs = 44100.0;
    int maxBlock = 512;

    // calibration
    std::atomic<float> calDb { 0.0f };
    std::array<std::atomic<float>, kNumSig> calSig;
    bool wasCalibrating = false;
    double sumDry = 0, sumWet = 0;
    long long calCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScorchProcessor)
};
