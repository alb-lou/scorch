#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AutoGain.h"

using namespace juce;

AudioProcessorValueTreeState::ParameterLayout ScorchProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto id = [] (const char* s) { return ParameterID { s, 1 }; };

    p.push_back (std::make_unique<AudioParameterChoice> (id ("style"), "Style",
                 StringArray { "Tape Tube", "Console", "Germanium", "Triode", "Pentode" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("drive"), "Drive", NormalisableRange<float> (0.f, 10.f, 0.01f), 3.f));
    p.push_back (std::make_unique<AudioParameterBool>  (id ("punish"), "Punish", false));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("tone"), "Tone", NormalisableRange<float> (-1.f, 1.f, 0.01f), 0.f));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("lowcut"), "Low Cut", NormalisableRange<float> (20.f, 1000.f, 1.f, 0.4f), 20.f));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("thump"), "Thump", NormalisableRange<float> (0.f, 10.f, 0.01f), 0.f));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("highcut"), "High Cut", NormalisableRange<float> (1000.f, 20000.f, 1.f, 0.4f), 20000.f));
    p.push_back (std::make_unique<AudioParameterBool>  (id ("steep"), "Steep", false));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("mix"), "Mix", NormalisableRange<float> (0.f, 100.f, 0.1f), 100.f));
    p.push_back (std::make_unique<AudioParameterFloat> (id ("output"), "Output", NormalisableRange<float> (-24.f, 12.f, 0.1f), 0.f));
    p.push_back (std::make_unique<AudioParameterBool>  (id ("autogain"), "Auto Gain", true));
    p.push_back (std::make_unique<AudioParameterBool>  (id ("calibrate"), "Calibrate", false,
                 AudioParameterBoolAttributes().withAutomatable (false)));
    return { p.begin(), p.end() };
}

ScorchProcessor::ScorchProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createParameterLayout())
{
    pStyle = apvts.getRawParameterValue ("style");   pDrive = apvts.getRawParameterValue ("drive");
    pPunish = apvts.getRawParameterValue ("punish"); pTone = apvts.getRawParameterValue ("tone");
    pLowCut = apvts.getRawParameterValue ("lowcut"); pThump = apvts.getRawParameterValue ("thump");
    pHighCut = apvts.getRawParameterValue ("highcut"); pSteep = apvts.getRawParameterValue ("steep");
    pMix = apvts.getRawParameterValue ("mix");       pOutput = apvts.getRawParameterValue ("output");
    pAuto = apvts.getRawParameterValue ("autogain"); pCal = apvts.getRawParameterValue ("calibrate");
    for (auto& s : calSig) s.store (0.0f);
}

bool ScorchProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto o = l.getMainOutputChannelSet();
    return (o == AudioChannelSet::mono() || o == AudioChannelSet::stereo()) && o == l.getMainInputChannelSet();
}

void ScorchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    fs = sampleRate;
    maxBlock = jmax (1, samplesPerBlock);
    const int nch = jmax (1, getTotalNumInputChannels());
    oversampling = std::make_unique<dsp::Oversampling<float>> ((size_t) nch, (size_t) 2,
                        dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
    oversampling->initProcessing ((size_t) samplesPerBlock);
    oversampling->reset();
    const int latency = (int) std::ceil (oversampling->getLatencyInSamples());
    setLatencySamples (latency);

    dsp::ProcessSpec spec { sampleRate, (uint32) samplesPerBlock, (uint32) nch };
    dryDelay.prepare (spec);
    dryDelay.setDelay ((float) latency);
    dryDelay.reset();

    dryBuf.setSize (nch, samplesPerBlock);
    for (auto* v : { &gDrive, &gComp, &gMix, &gOut }) v->assign ((size_t) samplesPerBlock, 0.0f);

    for (auto& c : cores) c.prepare (sampleRate * 4.0);
    for (auto& b : lowCut) b.reset();
    for (auto* arr : { &thump, &toneLo, &toneHi, &hc1, &hc2 }) for (auto& b : *arr) b.reset();
    lastStyle = -1;

    const double ramp = 0.03;
    driveSm.reset (sampleRate, ramp); compSm.reset (sampleRate, 0.05); mixSm.reset (sampleRate, ramp); outSm.reset (sampleRate, ramp);
    const bool punish = *pPunish > 0.5f;
    driveSm.setCurrentAndTargetValue ((float) sat::dbToLin (sat::driveToDb (*pDrive, punish)));
    compSm.setCurrentAndTargetValue (1.0f);
    mixSm.setCurrentAndTargetValue (*pMix * 0.01f);
    outSm.setCurrentAndTargetValue ((float) sat::dbToLin (*pOutput));
    wasCalibrating = false;
}

bool ScorchProcessor::sigMatches (const Sig& s) const
{
    for (int i = 0; i < kNumSig; ++i)
        if (std::abs (calSig[(size_t) i].load() - s[(size_t) i]) > 1e-4f) return false;
    return true;
}

void ScorchProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    // Hosts should never exceed the prepared block size, but if one does, process in safe chunks.
    const int total = buffer.getNumSamples();
    for (int pos = 0; pos < total; pos += maxBlock)
    {
        AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), pos, jmin (maxBlock, total - pos));
        processChunk (chunk);
    }
}

void ScorchProcessor::processChunk (AudioBuffer<float>& buffer)
{
    const int numCh = jmin (buffer.getNumChannels(), 2);
    const int n = buffer.getNumSamples();
    for (int c = getTotalNumInputChannels(); c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);
    if (n == 0 || oversampling == nullptr) return;

    if (n > (int) gDrive.size()) for (auto* v : { &gDrive, &gComp, &gMix, &gOut }) v->resize ((size_t) n);
    if (dryBuf.getNumSamples() < n || dryBuf.getNumChannels() < numCh) dryBuf.setSize (numCh, n, false, false, true);

    // ---- parameters
    const int   style  = (int) *pStyle;
    const float drive  = *pDrive;
    const bool  punish = *pPunish > 0.5f, steep = *pSteep > 0.5f, autoOn = *pAuto > 0.5f;
    const float tone = *pTone, lowCutHz = *pLowCut, thumpAmt = *pThump, highCutHz = *pHighCut;
    const bool  calReq = *pCal > 0.5f;

    if (style != lastStyle) { for (auto& c : cores) c.reset(); lastStyle = style; }

    const Sig sig { (float) style, drive, punish ? 1.f : 0.f, tone, lowCutHz, thumpAmt, highCutHz, steep ? 1.f : 0.f };

    // ---- calibration state machine
    if (calReq && ! wasCalibrating) { sumDry = sumWet = 0; calCount = 0; }
    if (! calReq && wasCalibrating)
    {
        if (calCount > (long long) (fs * 0.25) && sumDry > 1e-12 && sumWet > 1e-12)
        {
            calDb.store ((float) (10.0 * std::log10 (sumDry / sumWet)));
            for (int i = 0; i < kNumSig; ++i) calSig[(size_t) i].store (sig[(size_t) i]);
            calValid.store (true);
        }
    }
    wasCalibrating = calReq;
    calibrating.store (calReq);

    // ---- auto gain (static): calibrated value if the settings still match, otherwise the built-in curve
    float compDb = 0.0f; int src = 0;
    if (autoOn)
    {
        if (calValid.load() && sigMatches (sig)) { compDb = calDb.load(); src = 2; }
        else                                      { compDb = sat::lookupAutoGainDb (style, punish, drive); src = 1; }
    }
    displayCompDb.store (compDb);
    compSource.store (src);

    driveSm.setTargetValue ((float) sat::dbToLin (sat::driveToDb (drive, punish)));
    compSm.setTargetValue ((float) sat::dbToLin (compDb));
    mixSm.setTargetValue (*pMix * 0.01f);
    outSm.setTargetValue ((float) sat::dbToLin (*pOutput));
    for (int i = 0; i < n; ++i)
    {
        gDrive[(size_t) i] = driveSm.getNextValue(); gComp[(size_t) i] = compSm.getNextValue();
        gMix[(size_t) i] = mixSm.getNextValue();     gOut[(size_t) i] = outSm.getNextValue();
    }

    // ---- filter coefficients
    const bool useLowCut = lowCutHz > 20.5f, useThump = thumpAmt > 0.01f, useTone = std::abs (tone) > 0.005f, useHC = highCutHz < 19990.f;
    for (int c = 0; c < numCh; ++c)
    {
        if (useLowCut) lowCut[(size_t) c].highpass (fs, lowCutHz);
        if (useThump)  thump[(size_t) c].lowShelf (fs, 90.0, thumpAmt * 0.9);
        if (useTone)   { toneLo[(size_t) c].lowShelf (fs, 650.0, -tone * 5.0); toneHi[(size_t) c].highShelf (fs, 650.0, tone * 5.0); }
        if (useHC)     { hc1[(size_t) c].onePoleLP (fs, highCutHz); hc2[(size_t) c].lowpass (fs, highCutHz); }
    }

    // ---- pre-saturation: dry capture (latency-matched), low cut, thump, drive
    for (int c = 0; c < numCh; ++c)
    {
        float* d = buffer.getWritePointer (c);
        float* dry = dryBuf.getWritePointer (c);
        for (int i = 0; i < n; ++i)
        {
            const float x = d[i];
            if (calReq) sumDry += (double) x * x;
            dryDelay.pushSample (c, x);
            dry[i] = dryDelay.popSample (c);
            double y = x;
            if (useLowCut) y = lowCut[(size_t) c].process (y);
            if (useThump)  y = thump[(size_t) c].process (y);
            d[i] = (float) y * gDrive[(size_t) i];
        }
    }

    // ---- saturation at 4x
    dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numCh, (size_t) n);
    auto osBlock = oversampling->processSamplesUp (block);
    for (size_t c = 0; c < (size_t) numCh; ++c)
    {
        float* d = osBlock.getChannelPointer (c);
        auto& core = cores[c];
        for (size_t i = 0; i < osBlock.getNumSamples(); ++i) d[i] = core.process (style, d[i]);
    }
    oversampling->processSamplesDown (block);

    // ---- post: tone, high cut, calibration measure, gain, mix
    for (int c = 0; c < numCh; ++c)
    {
        float* d = buffer.getWritePointer (c);
        const float* dry = dryBuf.getReadPointer (c);
        for (int i = 0; i < n; ++i)
        {
            double y = d[i];
            if (useTone) y = toneHi[(size_t) c].process (toneLo[(size_t) c].process (y));
            if (useHC)   y = steep ? hc2[(size_t) c].process (y) : hc1[(size_t) c].process (y);
            if (calReq)  sumWet += y * y;
            const float m = gMix[(size_t) i];
            d[i] = (dry[i] * (1.0f - m) + (float) y * gComp[(size_t) i] * m) * gOut[(size_t) i];
        }
    }
    if (calReq) calCount += n;
}

AudioProcessorEditor* ScorchProcessor::createEditor() { return new ScorchEditor (*this); }

void ScorchProcessor::getStateInformation (MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("calValid", calValid.load(), nullptr);
    state.setProperty ("calDb", calDb.load(), nullptr);
    for (int i = 0; i < kNumSig; ++i) state.setProperty ("calSig" + String (i), calSig[(size_t) i].load(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void ScorchProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = ValueTree::fromXml (*xml);
            apvts.replaceState (vt);
            calDb.store ((float) (double) vt.getProperty ("calDb", 0.0));
            for (int i = 0; i < kNumSig; ++i) calSig[(size_t) i].store ((float) (double) vt.getProperty ("calSig" + String (i), 0.0));
            calValid.store ((bool) vt.getProperty ("calValid", false));
            if (auto* p = apvts.getParameter ("calibrate")) p->setValueNotifyingHost (0.0f);   // never resume mid-calibration
        }
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ScorchProcessor(); }
