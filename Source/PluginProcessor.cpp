#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    const juce::Identifier kRootId     { "TWISTED_BREAKS" };
    const juce::Identifier kMetaId     { "META" };
    const juce::Identifier kVersionId  { "stateVersion" };
    const juce::Identifier kMidiMapId  { "MIDI_MAP" };
    const juce::Identifier kPadId      { "PAD" };
    const juce::Identifier kIndexId    { "index" };
    const juce::Identifier kNoteId     { "note" };
}

TwistedBreaksProcessor::TwistedBreaksProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, kRootId, tb::params::createLayout())
{
    masterGainParam    = apvts.getRawParameterValue (tb::params::masterGain);
    masterLimiterParam = apvts.getRawParameterValue (tb::params::masterLimiter);
    masterPanicParam   = apvts.getRawParameterValue (tb::params::masterPanic);
    jassert (masterGainParam && masterLimiterParam && masterPanicParam);

    for (int pad = 0; pad < tb::kNumPads; ++pad)
    {
        auto& pp = padParams[(size_t) pad];
        using namespace tb::params;
        pp.gain    = apvts.getRawParameterValue (padId (pad, padGain));
        pp.pan     = apvts.getRawParameterValue (padId (pad, padPan));
        pp.pitch   = apvts.getRawParameterValue (padId (pad, padPitch));
        pp.start   = apvts.getRawParameterValue (padId (pad, padStart));
        pp.end     = apvts.getRawParameterValue (padId (pad, padEnd));
        pp.reverse = apvts.getRawParameterValue (padId (pad, padReverse));
        jassert (pp.gain && pp.pan && pp.pitch && pp.start && pp.end && pp.reverse);
    }

    for (auto* param : AudioProcessor::getParameters())
        param->addListener (this);

    samples.onStateChanged = [this] { markStateChanged(); };
}

TwistedBreaksProcessor::~TwistedBreaksProcessor()
{
    cancelPendingUpdate();
    for (auto* param : AudioProcessor::getParameters())
        param->removeListener (this);
}

//==============================================================================
void TwistedBreaksProcessor::parameterGestureChanged (int, bool gestureIsStarting)
{
    // Gestures only come from a person touching a control (never from host
    // automation playback), so this can't flood the host.
    if (! gestureIsStarting)
        markStateChanged();
}

void TwistedBreaksProcessor::markStateChanged()
{
    triggerAsyncUpdate();   // coalesces bursts into one host notification
}

void TwistedBreaksProcessor::handleAsyncUpdate()
{
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
}

//==============================================================================
bool TwistedBreaksProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Stereo out only. (Phase 5+ may add optional multi-out group buses.)
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet().isDisabled();
}

void TwistedBreaksProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Everything is allocated here, on the message thread, before audio runs.
    transport.prepare (sampleRate);
    engine.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (0);
}

void TwistedBreaksProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const auto& ts = transport.update (getPlayHead(), buffer.getNumSamples());
    transport.publish (bridge.transport);

    tb::AudioEngine::BlockParams p;
    p.masterGainDb   = masterGainParam->load (std::memory_order_relaxed);
    p.limiterEnabled = masterLimiterParam->load (std::memory_order_relaxed) > 0.5f;
    p.panicParam     = masterPanicParam->load (std::memory_order_relaxed) > 0.5f;

    constexpr auto relaxed = std::memory_order_relaxed;
    for (size_t pad = 0; pad < (size_t) tb::kNumPads; ++pad)
    {
        const auto& src = padParams[pad];
        auto& dst = p.pads[pad];
        dst.gainDb  = src.gain->load (relaxed);
        dst.pan     = src.pan->load (relaxed);
        dst.pitch   = src.pitch->load (relaxed);
        dst.start   = src.start->load (relaxed);
        dst.end     = src.end->load (relaxed);
        dst.reverse = src.reverse->load (relaxed) > 0.5f;
    }

    engine.process (buffer, midi, ts, p);

    midi.clear();   // instrument: nothing passes through
}

//==============================================================================
void TwistedBreaksProcessor::triggerPadFromUi (int pad, float velocity, bool noteOn)
{
    bridge.padEvents.push ({ pad, velocity, noteOn });
}

juce::AudioProcessorEditor* TwistedBreaksProcessor::createEditor()
{
    return new TwistedBreaksEditor (*this);
}

//==============================================================================
/*
    State layout (XML inside the binary blob Ableton stores in the .als):

    <TWISTED_BREAKS>                 <- APVTS parameter tree
      <PARAM id="master_gain" .../>
      ...
      <META stateVersion="1"/>
      <MIDI_MAP> <PAD index="0" note="36"/> ... </MIDI_MAP>
      <PADS> <PAD index="0" path="/Users/.../amen.wav"/> ... </PADS>
      (Phase 4: slices, Phase 7: PATTERNS, Phase 8: SNAPSHOTS)

    Per-pad gain / pan / pitch / start / end / reverse are parameters, so
    they're already in the APVTS part. A missing sample file never stops
    a set from opening: the pad shows MISSING and keeps its path.
    </TWISTED_BREAKS>

    stateVersion lets future builds migrate old sets instead of failing.
*/
void TwistedBreaksProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    state.removeChild (state.getChildWithName (kMetaId), nullptr);
    state.removeChild (state.getChildWithName (kMidiMapId), nullptr);
    state.removeChild (state.getChildWithName (tb::SampleManager::padsId), nullptr);

    juce::ValueTree meta (kMetaId);
    meta.setProperty (kVersionId, kStateVersion, nullptr);
    meta.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);
    state.appendChild (meta, nullptr);

    juce::ValueTree midiMap (kMidiMapId);
    for (int p = 0; p < tb::kNumPads; ++p)
    {
        juce::ValueTree pad (kPadId);
        pad.setProperty (kIndexId, p, nullptr);
        pad.setProperty (kNoteId,  engine.getPadNote (p), nullptr);
        midiMap.appendChild (pad, nullptr);
    }
    state.appendChild (midiMap, nullptr);
    state.appendChild (samples.toValueTree(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TwistedBreaksProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;   // unknown / corrupt blob: keep defaults rather than crash

    auto state = juce::ValueTree::fromXml (*xml);

    const int version = (int) state.getChildWithName (kMetaId).getProperty (kVersionId, 1);
    juce::ignoreUnused (version);   // migrations go here when version > 1 exists

    if (auto midiMap = state.getChildWithName (kMidiMapId); midiMap.isValid())
        for (const auto& pad : midiMap)
            engine.setPadNote ((int) pad.getProperty (kIndexId, -1),
                               (int) pad.getProperty (kNoteId, -1));

    samples.restoreFromValueTree (state.getChildWithName (tb::SampleManager::padsId));
    apvts.replaceState (state);
}

//==============================================================================
#if ! TB_TEST_BUILD
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TwistedBreaksProcessor();
}
#endif
