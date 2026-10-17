#include "PluginProcessor.h"
#include "Core/FxDefs.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <vector>

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
    seqRunParam    = apvts.getRawParameterValue (tb::params::seqRun);
    seqLengthParam = apvts.getRawParameterValue (tb::params::seqLength);
    seqResParam    = apvts.getRawParameterValue (tb::params::seqRes);
    seqQuantParam  = apvts.getRawParameterValue (tb::params::seqQuant);
    seqFillParam   = apvts.getRawParameterValue (tb::params::seqFill);
    for (int m = 0; m < tb::macro::kNumMacros; ++m)
        macroOnParam[(size_t) m] = apvts.getRawParameterValue (tb::macro::paramId (m));
    macroAmountParam = apvts.getRawParameterValue (tb::params::macroAmount);
    xfadeParam       = apvts.getRawParameterValue (tb::params::xfade);
    xfadeCurveParam  = apvts.getRawParameterValue (tb::params::xfadeCurve);
    sceneViewParam   = apvts.getRawParameterValue (tb::params::sceneView);
    macroGroupParam  = apvts.getRawParameterValue (tb::params::macroGroup);
    macroLatchParam  = apvts.getRawParameterValue (tb::params::macroLatch);
    jassert (seqFillParam != nullptr);
    jassert (seqRunParam && seqLengthParam && seqResParam && seqQuantParam);
    bankActiveParam = apvts.getRawParameterValue (tb::params::bankActive);
    bankAtParam     = apvts.getRawParameterValue (tb::params::bankAt);
    jassert (bankActiveParam && bankAtParam);

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
        pp.mode    = apvts.getRawParameterValue (padId (pad, padMode));
        pp.sync    = apvts.getRawParameterValue (padId (pad, padSync));
        pp.rate    = apvts.getRawParameterValue (padId (pad, padRate));
        pp.choke   = apvts.getRawParameterValue (padId (pad, padChoke));
        pp.velSens = apvts.getRawParameterValue (padId (pad, padVelSens));
        pp.mute    = apvts.getRawParameterValue (padId (pad, padMute));
        pp.solo    = apvts.getRawParameterValue (padId (pad, padSolo));
        pp.slice   = apvts.getRawParameterValue (padId (pad, padSlice));
        pp.swap    = apvts.getRawParameterValue (padId (pad, padSwap));
        pp.swapAt  = apvts.getRawParameterValue (padId (pad, padSwapAt));
        pp.group   = apvts.getRawParameterValue (padId (pad, padGroup));
        pp.fixVel  = apvts.getRawParameterValue (padId (pad, padFixVel));
        pp.sliceSeq  = apvts.getRawParameterValue (padId (pad, padSliceSeq));
        pp.sliceSlot = apvts.getRawParameterValue (padId (pad, padSliceSlot));
        pp.followBank = apvts.getRawParameterValue (padId (pad, padFollowBank));
        pp.character  = apvts.getRawParameterValue (padId (pad, padCharacter));
        pp.launch     = apvts.getRawParameterValue (padId (pad, padLaunch));
        pp.seqLen     = apvts.getRawParameterValue (padId (pad, padSeqLen));
        pp.swapFrom   = apvts.getRawParameterValue (padId (pad, padSwapFrom));
        pp.press      = apvts.getRawParameterValue (padId (pad, padPress));
        pp.stepsWhole = apvts.getRawParameterValue (padId (pad, padStepsWhole));
        pp.fit        = apvts.getRawParameterValue (padId (pad, padFit));
        pp.layers     = apvts.getRawParameterValue (padId (pad, padLayers));
        jassert (pp.fit && pp.layers);
        jassert (pp.gain && pp.pan && pp.pitch && pp.start && pp.end && pp.reverse
                 && pp.mode && pp.sync && pp.rate && pp.choke && pp.velSens && pp.mute && pp.solo && pp.slice
                 && pp.swap && pp.swapAt && pp.group && pp.fixVel && pp.sliceSeq && pp.sliceSlot && pp.followBank && pp.character
                 && pp.launch && pp.seqLen && pp.swapFrom && pp.press && pp.stepsWhole);
    }

    for (int g = 0; g < tb::kNumGroups; ++g)
    {
        using namespace tb::params;
        auto& gp = groupParams[(size_t) g];
        gp.level  = apvts.getRawParameterValue (groupId (g, grpLevel));
        gp.pan    = apvts.getRawParameterValue (groupId (g, grpPan));
        gp.mute   = apvts.getRawParameterValue (groupId (g, grpMute));
        gp.solo   = apvts.getRawParameterValue (groupId (g, grpSolo));
        gp.bypass = apvts.getRawParameterValue (groupId (g, grpBypass));
        jassert (gp.level && gp.pan && gp.mute && gp.solo && gp.bypass);
        for (int f = 0; f < tb::fx::kNumFx; ++f)
        {
            gp.fxOn[(size_t) f] = apvts.getRawParameterValue (tb::fx::paramId (g, f));
            jassert (gp.fxOn[(size_t) f] != nullptr);
            for (int k = 0; k < tb::fx::def (f).numParams; ++k)
            {
                gp.fxParam[(size_t) f][(size_t) k] = apvts.getRawParameterValue (tb::fx::paramId (g, f, k));
                jassert (gp.fxParam[(size_t) f][(size_t) k] != nullptr);
            }
        }
    }

    for (auto* param : AudioProcessor::getParameters())
        param->addListener (this);

    samples.onStateChanged = [this] { markStateChanged(); };
    bank      = std::make_unique<tb::BreakBank> (samples);
    snapshots = std::make_unique<tb::SnapshotManager> (*this);
    mutator   = std::make_unique<tb::Mutator> (*this);
    resampler = std::make_unique<tb::Resampler> (*this);
    padOps    = std::make_unique<tb::PadOps> (*this);
    presets   = std::make_unique<tb::PresetManager> (*this);
    push      = std::make_unique<tb::PushController> (*this);
    mikro     = std::make_unique<tb::MikroTester>();
    mikroMode = std::make_unique<tb::MikroController> (*this);
    mikroMode->onSettingsChanged = [this] { markStateChanged(); };
    lights.onSettingsChanged = [this] { markStateChanged(); };
    push->onSettingsChanged  = [this] { markStateChanged(); };
}

TwistedBreaksProcessor::~TwistedBreaksProcessor()
{
    push.reset();      // first: it uses the mutator and resampler
    mikroMode.reset(); // (it uses them too, and the snapshots)
    mikro.reset();
    // Frames still handed over to the audio thread (audio has stopped now).
    for (auto* f : { armedFrame.exchange (nullptr), overrideFrame.exchange (nullptr), appliedFrame.exchange (nullptr) })
        if (f != nullptr) snapshots->adoptForDeletion (f);
    snapshots.reset();
    for (auto* f : { armedMutation.exchange (nullptr), appliedMutation.exchange (nullptr) })
        if (f != nullptr) mutator->adoptForDeletion (f);
    mutator.reset();
    resampler.reset();
    bank.reset();

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

int TwistedBreaksProcessor::spreadSlices (int pad)
{
    const int filled = samples.spreadToPads (pad);
    using namespace tb::params;

    auto set = [this] (int p, const char* name, float plain)
    {
        if (auto* prm = apvts.getParameter (padId (p, name)))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
            prm->endChangeGesture();
        }
    };
    auto get = [this, pad] (const char* name) { return apvts.getRawParameterValue (padId (pad, name))->load(); };

    const float gain = get (padGain), pitch = get (padPitch), rev = get (padReverse),
                vel = get (padVelSens), choke = get (padChoke);

    for (int i = 0; i < filled; ++i)
    {
        const int target = pad + i;
        set (target, padMode, (float) modeSlice);
        set (target, padSlice, (float) (i + 1));
        if (target == pad) continue;
        set (target, padGain, gain);
        set (target, padPitch, pitch);
        set (target, padReverse, rev);
        set (target, padVelSens, vel);
        set (target, padChoke, choke);
        set (target, padStart, 0.0f);
        set (target, padEnd, 1.0f);
    }
    markStateChanged();
    return filled;
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
    midiFirst.ensureSize (8192);
    midiSecond.ensureSize (8192);
    setLatencySamples (0);
}

namespace
{
    /** Rounds away float noise smaller than `resolution`. */
    float cleanValue (float v, float resolution) noexcept
    {
        return std::round (v / resolution) * resolution;
    }

    /** Pan: exact centre and exact hard left / right. */
    float cleanPan (float v) noexcept
    {
        if (std::abs (v) < 1.0e-4f)        return 0.0f;
        if (v >  1.0f - 1.0e-4f)           return 1.0f;
        if (v < -1.0f + 1.0e-4f)           return -1.0f;
        return v;
    }
}

template <typename Load>
void TwistedBreaksProcessor::fillParams (tb::AudioEngine::BlockParams& p, Load&& ld) const
{
    p.masterGainDb   = ld (masterGainParam);
    p.limiterEnabled = ld (masterLimiterParam) > 0.5f;
    p.panicParam     = ld (masterPanicParam) > 0.5f;
    p.seqRun         = ld (seqRunParam) > 0.5f;
    p.seqLength      = juce::roundToInt (ld (seqLengthParam));
    p.seqRes         = juce::roundToInt (ld (seqResParam));
    p.seqQuant       = juce::roundToInt (ld (seqQuantParam));
    p.seqFill        = ld (seqFillParam) > 0.5f;
    p.xfade      = ld (xfadeParam);
    p.xfadeCurve = juce::roundToInt (ld (xfadeCurveParam));
    p.sceneView  = juce::roundToInt (ld (sceneViewParam));
    p.macroOn = 0;
    for (int m = 0; m < tb::macro::kNumMacros; ++m)
        if (ld (macroOnParam[(size_t) m]) > 0.5f) p.macroOn |= 1u << m;
    p.macroAmount = ld (macroAmountParam) / 100.0f;
    p.macroGroup  = juce::roundToInt (ld (macroGroupParam));
    p.macroLatch  = ld (macroLatchParam) > 0.5f;
    p.bankActive     = juce::jlimit (0, 7, juce::roundToInt (ld (bankActiveParam)) - 1);
    p.bankAt         = juce::roundToInt (ld (bankAtParam));

    for (size_t pad = 0; pad < (size_t) tb::kNumPads; ++pad)
    {
        const auto& src = padParams[pad];
        auto& dst = p.pads[pad];
        // Parameter hygiene: snapping a value to its step (e.g. 0.01) can
        // leave float rounding noise, and it differs between CPUs (Apple
        // Silicon fuses the multiply-add). Clean it so "pitch 0" is exactly
        // unity speed (loops stay sample-exact) and "pan hard right" is
        // exactly silent on the left.
        dst.gainDb  = cleanValue (ld (src.gain), 1.0e-3f);
        dst.pan     = cleanPan (ld (src.pan));
        dst.pitch   = cleanValue (ld (src.pitch), 1.0e-4f);
        dst.start   = ld (src.start);
        dst.end     = ld (src.end);
        dst.reverse = ld (src.reverse) > 0.5f;
        dst.mode    = juce::roundToInt (ld (src.mode));
        dst.sync    = ld (src.sync) > 0.5f;
        dst.rate    = juce::roundToInt (ld (src.rate));
        dst.choke   = juce::roundToInt (ld (src.choke));
        dst.velSens = ld (src.velSens);
        dst.mute    = ld (src.mute) > 0.5f;
        dst.solo    = ld (src.solo) > 0.5f;
        dst.slice   = juce::roundToInt (ld (src.slice));
        dst.swap    = juce::roundToInt (ld (src.swap));
        dst.swapAt  = juce::roundToInt (ld (src.swapAt));
        dst.group   = juce::jlimit (0, tb::kNumGroups - 1, juce::roundToInt (ld (src.group)));
        dst.fixVel  = ld (src.fixVel) > 0.5f;
        dst.sliceSeq  = ld (src.sliceSeq) > 0.5f;
        dst.sliceSlot = juce::jlimit (0, 7, juce::roundToInt (ld (src.sliceSlot)) - 1);
        dst.followBank = ld (src.followBank) > 0.5f;
        dst.character  = juce::roundToInt (ld (src.character));
        dst.launch     = juce::roundToInt (ld (src.launch));
        dst.seqLen     = juce::roundToInt (ld (src.seqLen));
        dst.swapFrom   = juce::roundToInt (ld (src.swapFrom));
        dst.press      = juce::roundToInt (ld (src.press));
        dst.stepsWhole = ld (src.stepsWhole) > 0.5f;
        dst.fit        = ld (src.fit) > 0.5f;
        dst.layerMode  = juce::jlimit (0, 3, juce::roundToInt (ld (src.layers)));
    }

    for (int g = 0; g < tb::kNumGroups; ++g)
    {
        const auto& src = groupParams[(size_t) g];
        auto& dst = p.groups[(size_t) g];
        dst.levelDb = cleanValue (ld (src.level), 1.0e-3f);
        dst.pan     = cleanPan (ld (src.pan));
        dst.mute    = ld (src.mute) > 0.5f;
        dst.solo    = ld (src.solo) > 0.5f;
        dst.bypass  = ld (src.bypass) > 0.5f;
        for (int f = 0; f < tb::fx::kNumFx; ++f)
        {
            dst.fxOn[(size_t) f] = ld (src.fxOn[(size_t) f]) > 0.5f;
            for (int k = 0; k < 3; ++k)
                if (auto* a = src.fxParam[(size_t) f][(size_t) k])
                    dst.fxParam[(size_t) f][(size_t) k] = ld (a);
        }
    }

}

void TwistedBreaksProcessor::fillParamsFrom (tb::AudioEngine::BlockParams& p, const std::function<float (const std::atomic<float>*)>& source) const
{
    fillParams (p, source);
}

int TwistedBreaksProcessor::switchOffset (const tb::TransportState& ts, int quant, int n, int bars) noexcept
{
    // NOW (or Live stopped): this block's first sample. Otherwise the next
    // beat / bar line (or every `bars` bars from the song start), if it falls
    // in this block (-1: not yet).
    if (quant == tb::params::switchNow || ! ts.isPlaying || ts.ppqPerSample <= 0.0) return 0;
    bars = juce::jmax (1, bars);
    const double p0     = ts.ppqAtBlockStart;
    const double unit   = quant == tb::params::switchBeat ? 1.0 : ts.ppqPerBar() * bars;
    const double origin = quant == tb::params::switchBeat ? 0.0
                        : ts.ppqOfBarStart - (double) (((ts.barIndex % bars) + bars) % bars) * ts.ppqPerBar();
    const double next   = origin + std::ceil ((p0 - origin) / unit - 1.0e-9) * unit;
    const int off = (int) std::lround ((next - p0) / ts.ppqPerSample);

    // A loop brace / relocate that lands right on such a boundary: it's now.
    if (ts.jumped && std::abs (next - p0) < 1.0e-3) return 0;
    return off < n ? juce::jmax (0, off) : -1;
}

void TwistedBreaksProcessor::applyMutationFrame (tb::MutationFrame* f) noexcept
{
    f->applyTo (bridge.seq, bridge.sliceSeq);
    auto* expected = f;
    armedMutation.compare_exchange_strong (expected, nullptr);
    appliedMutation.store (f);
}

void TwistedBreaksProcessor::applySnapshotFrame (tb::SnapshotFrame* f) noexcept
{
    // Audio thread, at the boundary sample: samples + markers, both
    // sequencers, then the parameter override until the message thread has
    // written the real parameters.
    engine.applySnapshotPads (f->raw, f->slices);
    for (int p = 0; p < tb::kNumPads; ++p)
    {
        for (int k = 0; k < tb::SequencerData::kSteps; ++k) bridge.seq.setPacked (p, k, f->seq[(size_t) p][(size_t) k]);
        for (int sl = 0; sl < tb::SliceSeqData::kSlots; ++sl)
        {
            bridge.sliceSeq.setPackedSlot (p, sl, f->sliceSlots[(size_t) p][(size_t) sl]);
            for (int k = 0; k < tb::SliceSeqData::kSteps; ++k)
                bridge.sliceSeq.setPackedStep (p, sl, k, f->sliceSteps[(size_t) p][(size_t) sl][(size_t) k]);
        }
    }
    bridge.seq.bumpVersion();
    bridge.sliceSeq.bumpVersion();

    auto* expected = f;
    armedFrame.compare_exchange_strong (expected, nullptr);
    overrideFrame.store (f);
    appliedFrame.store (f);
}

tb::SnapshotFrame* TwistedBreaksProcessor::armSnapshotFrame (tb::SnapshotFrame* f) noexcept
{
    return armedFrame.exchange (f);
}

void TwistedBreaksProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    const auto& ts = transport.update (getPlayHead(), n);
    transport.publish (bridge.transport);

    // Snapshot / mutation due in this block? 0 = now, > 0 = part-way through, -1 = not yet.
    auto* armed = armedFrame.load();
    const int switchAt = armed != nullptr ? switchOffset (ts, armed->quant, n) : -1;
    if (switchAt == 0) applySnapshotFrame (armed);
    auto* mutation = armedMutation.load();
    const int mutateAt = mutation != nullptr ? switchOffset (ts, mutation->quant, n, mutation->bars) : -1;
    if (mutateAt == 0) applyMutationFrame (mutation);

    auto live = [] (const std::atomic<float>* a) { return a->load (std::memory_order_relaxed); };
    auto overlay = [this] (tb::AudioEngine::BlockParams& bp)
    {
        if (auto* f = overrideFrame.load())
        {
            bp.pads = f->params.pads;
            bp.groups = f->params.groups;
            bp.seqRun = f->params.seqRun; bp.seqLength = f->params.seqLength;
            bp.seqRes = f->params.seqRes; bp.seqQuant = f->params.seqQuant;
        }
    };

    tb::AudioEngine::BlockParams p;
    fillParams (p, live);
    overlay (p);

    // Split the block at the boundaries, so each change lands on its exact sample.
    int cuts[3]; int numCuts = 0;
    if (switchAt > 0) cuts[numCuts++] = switchAt;
    if (mutateAt > 0 && mutateAt != switchAt) cuts[numCuts++] = mutateAt;
    if (numCuts == 2 && cuts[1] < cuts[0]) std::swap (cuts[0], cuts[1]);
    cuts[numCuts++] = n;

    if (numCuts == 1)
    {
        engine.process (buffer, midi, ts, p);
    }
    else
    {
        int pos = 0;
        for (int c = 0; c < numCuts; ++c)
        {
            const int end = cuts[c];
            juce::AudioBuffer<float> part (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), pos, end - pos);
            midiFirst.clear();
            for (const auto m : midi)
                if (m.samplePosition >= pos && (m.samplePosition < end || end == n))
                    midiFirst.addEvent (m.data, m.numBytes, m.samplePosition - pos);
            auto tsPart = ts;
            tsPart.ppqAtBlockStart += pos * ts.ppqPerSample;
            tsPart.numSamples = end - pos;
            if (pos > 0) tsPart.justStarted = tsPart.justStopped = tsPart.jumped = false;
            engine.process (part, midiFirst, tsPart, p);

            if (end < n)
            {
                if (end == switchAt) { applySnapshotFrame (armed); overlay (p); }
                if (end == mutateAt) applyMutationFrame (mutation);
            }
            pos = end;
        }
    }

    midi.clear();   // instrument: nothing passes through
    blockEpoch.fetch_add (1, std::memory_order_release);
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
    state.removeChild (state.getChildWithName (tb::PadLights::lightsId), nullptr);
    state.removeChild (state.getChildWithName ("FX"), nullptr);
    state.removeChild (state.getChildWithName (tb::SequencerData::seqId), nullptr);
    state.removeChild (state.getChildWithName (tb::SliceSeqData::sliceSeqId), nullptr);
    state.removeChild (state.getChildWithName (tb::SnapshotManager::snapshotsId), nullptr);
    state.removeChild (state.getChildWithName (tb::BreakBank::bankId), nullptr);

    juce::ValueTree meta (kMetaId);
    meta.setProperty (kVersionId, kStateVersion, nullptr);
    meta.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);
    if (presets != nullptr)
    {
        meta.setProperty ("kitName", presets->getKitName(), nullptr);
        meta.setProperty ("kitFile", presets->getCurrentKitFile().getFullPathName(), nullptr);
    }
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
    state.appendChild (lights.toValueTree(), nullptr);
    state.appendChild (push->toValueTree(), nullptr);
    state.appendChild (mikroMode->toValueTree(), nullptr);

    state.appendChild (bridge.seq.toValueTree(), nullptr);
    state.appendChild (bridge.sliceSeq.toValueTree(), nullptr);
    state.appendChild (snapshots->toValueTree(), nullptr);
    state.appendChild (bank->toValueTree(), nullptr);

    juce::ValueTree fxState ("FX");
    fxState.setProperty ("lockedMask", (juce::int64) engine.getFxLockedMask(), nullptr);
    fxState.setProperty ("group", engine.getFxTargetGroup(), nullptr);
    state.appendChild (fxState, nullptr);

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
    if (padOps != nullptr) padOps->reset();             // a new project: no undo back into the old one
    if (presets != nullptr)
    {
        const auto m = state.getChildWithName (kMetaId);
        const auto name = m.getProperty ("kitName").toString(), file = m.getProperty ("kitFile").toString();
        juce::WeakReference<tb::PresetManager> weak (presets.get());
        auto apply = [weak, name, file] { if (auto* pm = weak.get()) pm->restoreKitName (name, file); };
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr && juce::MessageManager::getInstance()->isThisTheMessageThread()) apply();
        else juce::MessageManager::callAsync (apply);
    }
    lights.fromValueTree (state.getChildWithName (tb::PadLights::lightsId));
    push->fromValueTree (state.getChildWithName (tb::PushController::pushId));
    mikroMode->fromValueTree (state.getChildWithName (tb::MikroController::mikroId));
    bridge.seq.fromValueTree (state.getChildWithName (tb::SequencerData::seqId));
    bridge.sliceSeq.fromValueTree (state.getChildWithName (tb::SliceSeqData::sliceSeqId));
    {
        // Snapshots + bank live on the message thread.
        auto snaps = state.getChildWithName (tb::SnapshotManager::snapshotsId).createCopy();
        auto bk    = state.getChildWithName (tb::BreakBank::bankId).createCopy();
        auto restore = [this, snaps, bk] { snapshots->fromValueTree (snaps); bank->fromValueTree (bk); };
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
             && juce::MessageManager::getInstance()->isThisTheMessageThread())
            restore();
        else
            juce::MessageManager::callAsync (restore);
    }
    if (auto fxState = state.getChildWithName ("FX"); fxState.isValid())
    {
        engine.setFxLockedMask ((uint32_t) (juce::int64) fxState.getProperty ("lockedMask", (juce::int64) tb::fx::kDefaultLockedMask));
        engine.setFxTargetGroup ((int) fxState.getProperty ("group", 0));
    }
    apvts.replaceState (state);
}

//==============================================================================
void TwistedBreaksProcessor::setFxLocked (int fxId, bool locked)
{
    if (! juce::isPositiveAndBelow (fxId, tb::fx::kNumFx) || isFxLocked (fxId) == locked) return;
    const uint32_t bit = 1u << fxId;
    engine.setFxLockedMask (locked ? (engine.getFxLockedMask() | bit) : (engine.getFxLockedMask() & ~bit));
    markStateChanged();
}

void TwistedBreaksProcessor::allFxOff()
{
    for (int g = 0; g < tb::kNumGroups; ++g)
        for (int f = 0; f < tb::fx::kNumFx; ++f)
            if (auto* prm = apvts.getParameter (tb::fx::paramId (g, f)))
                if (prm->getValue() > 0.5f)
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (0.0f);
                    prm->endChangeGesture();
                }
}

int TwistedBreaksProcessor::sliceSlotOf (int pad) const
{
    return juce::jlimit (0, 7, juce::roundToInt (apvts.getRawParameterValue (tb::params::padId (pad, tb::params::padSliceSlot))->load()) - 1);
}

void TwistedBreaksProcessor::materializeSliceSlot (int pad, int slot)
{
    auto& data = bridge.sliceSeq;
    auto settings = data.getSlot (pad, slot);
    if (settings.written) return;

    const auto info = samples.getInfo (pad);
    std::array<tb::SliceStep, tb::SliceSeqData::kSteps> pat {};
    int length = settings.length;
    if (info.buffer != nullptr)
    {
        const double beats = tb::PadVoice::loopBeatsFor ((double) info.buffer->getNumSamples() / info.buffer->sourceSampleRate);
        length = tb::SliceSeqData::defaultPattern (info.slices, info.buffer->getNumSamples(), info.buffer->sourceSampleRate,
                                                   tb::kSliceRateBeats[settings.rate], beats, pat);
    }
    for (int k = 0; k < tb::SliceSeqData::kSteps; ++k) data.setStep (pad, slot, k, pat[(size_t) k]);
    settings.length = length;
    settings.written = true;
    data.setSlot (pad, slot, settings);
}

void TwistedBreaksProcessor::sliceSeqOp (int pad, SliceOp op, juce::Random& rnd)
{
    auto& data = bridge.sliceSeq;
    const int slot = sliceSlotOf (pad);
    if (op == SliceOp::reset) { data.resetSlot (pad, slot); markStateChanged(); return; }

    materializeSliceSlot (pad, slot);
    const int L = data.getSlot (pad, slot).length;
    std::array<tb::SliceStep, tb::SliceSeqData::kSteps> st {};
    for (int k = 0; k < L; ++k) st[(size_t) k] = data.getStep (pad, slot, k);

    const auto info = samples.getInfo (pad);
    const int sliceCount = info.slices.isEmpty() ? 16 : info.slices.count;

    switch (op)
    {
        case SliceOp::shuffle:
        {
            // Swap hits around, but leave every beat (steps 1, 5, 9 ...) alone,
            // so the kicks / snares that carry the groove stay put.
            std::vector<int> pos;
            for (int k = 0; k < L; ++k) if (k % 4 != 0 && st[(size_t) k].kind == tb::SliceStep::play) pos.push_back (k);
            for (int i = (int) pos.size() - 1; i > 0; --i)
                std::swap (st[(size_t) pos[(size_t) i]], st[(size_t) pos[(size_t) rnd.nextInt (i + 1)]]);
            break;
        }
        case SliceOp::random:
            for (int k = 0; k < L; ++k)
                if (k % 4 != 0 && st[(size_t) k].kind == tb::SliceStep::play)
                    st[(size_t) k].slice = rnd.nextInt (sliceCount);
            break;
        case SliceOp::reverseOrder: std::reverse (st.begin(), st.begin() + L); break;
        case SliceOp::half:         for (int k = L / 2; k < L; ++k) st[(size_t) k] = st[(size_t) (k - L / 2)]; break;
        case SliceOp::rotateLeft:   std::rotate (st.begin(), st.begin() + 1, st.begin() + L); break;
        case SliceOp::rotateRight:  std::rotate (st.begin(), st.begin() + L - 1, st.begin() + L); break;
        case SliceOp::reset:        break;
    }
    for (int k = 0; k < L; ++k) data.setStep (pad, slot, k, st[(size_t) k]);
    markStateChanged();
}

void TwistedBreaksProcessor::setFxGroup (int g)
{
    g = juce::jlimit (0, tb::kNumGroups - 1, g);
    if (g == engine.getFxTargetGroup()) return;
    engine.setFxTargetGroup (g);
    markStateChanged();
}

//==============================================================================
#if ! TB_TEST_BUILD
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TwistedBreaksProcessor();
}
#endif
