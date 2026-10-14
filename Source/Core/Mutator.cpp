#include "Core/Mutator.h"
#include "Core/Parameters.h"
#include "Core/FxDefs.h"
#include "PluginProcessor.h"

namespace tb
{
    namespace
    {
        bool roll (juce::Random& r, float p) { return p > 0.0f && r.nextFloat() < p; }
        template <size_t N> int pick (juce::Random& r, const int (&v)[N]) { return v[(size_t) r.nextInt ((int) N)]; }

        constexpr int kRatchets[] = { 2, 2, 3, 4 };
        constexpr int kPitches[]  = { 12, -12, 7, -5, 5, -7, 2, -2, 12 };
        constexpr int kSlicePitches[] = { 12, -12, 7, -5, 5, -7, 2, -2 };
        constexpr int kFxThrows[] = { fx::stutter, fx::repeat, fx::reverse, fx::pitch, fx::gate, fx::crush };
    }

    Mutator::Mutator (TwistedBreaksProcessor& p) : processor (p), rng (juce::Time::currentTimeMillis())
    {
        startTimerHz (30);
    }

    Mutator::~Mutator()
    {
        stopTimer();
    }

    //==========================================================================
    //  The rules
    //==========================================================================
    void Mutator::mutateLane (Lane& out, const Lane& orig, int L, int spb, int slices, float a, uint32_t t, juce::Random& r)
    {
        out = orig;
        L = juce::jlimit (1, SequencerData::kSteps, L);
        spb = juce::jmax (1, spb);
        a = juce::jlimit (0.0f, 1.0f, a);
        const auto on = [&] (uint32_t bit) { return ((t >> bit) & 1u) != 0; };

        int hits = 0; float velSum = 0.0f;
        for (int k = 0; k < L; ++k)
            if (orig[(size_t) k].on) { ++hits; velSum += (float) orig[(size_t) k].velocity; }
        if (hits == 0 || a <= 0.0f) return;                 // nothing to vary: never invent a lane
        const float avgVel = velSum / (float) hits;

        for (int k = 0; k < L; ++k)
        {
            const auto& o = orig[(size_t) k];
            auto& s = out[(size_t) k];
            const bool beat     = k % spb == 0;
            const bool barStart = k % (spb * 4) == 0;
            const bool lastBeat = k % (spb * 4) >= spb * 3;
            const bool anchor   = beat && o.on;

            // PATTERN: off-beat hits may drop out; ghosts appear next to hits.
            if (on (params::mutTPattern) && ! beat)
            {
                if (o.on)
                {
                    if (roll (r, 0.3f * a)) s = {};
                }
                else
                {
                    const bool nextToHit = orig[(size_t) ((k + L - 1) % L)].on || orig[(size_t) ((k + 1) % L)].on;
                    if (nextToHit && roll (r, 0.35f * a))
                    {
                        s = {};
                        s.on = true;
                        s.velocity = juce::jlimit (1, 127, juce::roundToInt (avgVel * (0.45f + 0.25f * r.nextFloat())));
                    }
                }
            }
            if (! s.on) continue;

            if (on (params::mutTRatchet) && ! barStart && roll (r, (lastBeat ? 0.4f : 0.15f) * a))
                s.ratchet = pick (r, kRatchets);

            if (anchor) continue;                            // the beat stays as written
            if (on (params::mutTSlice) && slices > 1 && roll (r, 0.3f * a))
                s.slice = 1 + r.nextInt (juce::jmin (16, slices));
            if (on (params::mutTPitch) && roll (r, 0.2f * a))
                s.pitch = pick (r, kPitches);
            if (on (params::mutTReverse) && roll (r, 0.15f * a))
                s.rev = ! s.rev;
            if (on (params::mutTFx) && lastBeat && roll (r, 0.25f * a))
                s.fx = pick (r, kFxThrows) + 1;
            if (on (params::mutTChance) && s.probability == 100 && roll (r, 0.3f * a))
                s.probability = 50 + 5 * r.nextInt (9);
        }
    }

    void Mutator::mutateSlices (Slots& out, const Slots& orig, int L, int spb, int count, float a, uint32_t t, juce::Random& r)
    {
        out = orig;
        L = juce::jlimit (1, SliceSeqData::kSteps, L);
        spb = juce::jmax (1, spb);
        a = juce::jlimit (0.0f, 1.0f, a);
        count = juce::jlimit (1, 16, count <= 0 ? 16 : count);
        if (a <= 0.0f) return;
        const auto on = [&] (uint32_t bit) { return ((t >> bit) & 1u) != 0; };

        // SLICES: off-beat steps trade slices (the break's own material, rearranged).
        std::array<int, SliceSeqData::kSteps> offbeat {};
        int n = 0;
        for (int k = 0; k < L; ++k)
            if (k % spb != 0 && orig[(size_t) k].kind == SliceStep::play) offbeat[(size_t) n++] = k;
        if (on (params::mutTSlice))
            for (int i = 0; i < n; ++i)
            {
                auto& s = out[(size_t) offbeat[(size_t) i]];
                if (n >= 2 && roll (r, 0.4f * a))
                {
                    int j = r.nextInt (n - 1);
                    if (j >= i) ++j;
                    std::swap (s.slice, out[(size_t) offbeat[(size_t) j]].slice);
                }
                else if (roll (r, 0.15f * a))
                {
                    s.slice = r.nextInt (count);
                }
            }

        // PATTERN: tied steps re-hit the slice that's ringing (repeats); a few off-beat hits rest.
        if (on (params::mutTPattern))
        {
            int ringing = -1;
            for (int k = 0; k < L; ++k)
            {
                const auto& o = orig[(size_t) k];
                auto& s = out[(size_t) k];
                if (k % spb != 0)
                {
                    if (o.kind == SliceStep::tie && ringing >= 0 && roll (r, 0.25f * a))
                    {
                        s = SliceStep::playing (ringing);
                        s.velocity = juce::jlimit (1, 127, juce::roundToInt (100.0f * (0.6f + 0.25f * r.nextFloat())));
                    }
                    else if (o.kind == SliceStep::play && roll (r, 0.12f * a))
                    {
                        s.kind = SliceStep::rest;
                    }
                }
                if (s.kind == SliceStep::play) ringing = s.slice;
            }
        }

        for (int k = 0; k < L; ++k)
        {
            auto& s = out[(size_t) k];
            if (s.kind != SliceStep::play) continue;
            const bool barStart = k % (spb * 4) == 0;
            const bool lastBeat = k % (spb * 4) >= spb * 3;
            if (on (params::mutTRatchet) && ! barStart && roll (r, (lastBeat ? 0.4f : 0.15f) * a))
                s.ratchet = pick (r, kRatchets);
            if (k % spb == 0) continue;                      // the beat stays as written
            if (on (params::mutTPitch) && roll (r, 0.2f * a))   s.pitch = pick (r, kSlicePitches);
            if (on (params::mutTReverse) && roll (r, 0.15f * a)) s.rev = ! s.rev;
            if (on (params::mutTChance) && s.chance == 100 && roll (r, 0.3f * a)) s.chance = 50 + 5 * r.nextInt (9);
        }
    }

    //==========================================================================
    //  State
    //==========================================================================
    int Mutator::stepsPerBeatMain() const
    {
        const int res = juce::jlimit (0, 2, juce::roundToInt (processor.getParameters().getRawParameterValue (params::seqRes)->load()));
        return juce::roundToInt (1.0 / params::seqResPpq[res]);
    }

    uint32_t Mutator::sliceSeqPads() const
    {
        uint32_t m = 0;
        auto& apvts = processor.getParameters();
        for (int p = 0; p < kNumPads; ++p)
            if (apvts.getRawParameterValue (params::padId (p, params::padSliceSeq))->load() > 0.5f
                && processor.getSamples().getInfo (p).buffer != nullptr)
                m |= 1u << p;
        return m;
    }

    uint32_t Mutator::padsInScope() const
    {
        const bool all = processor.getParameters().getRawParameterValue (params::mutScope)->load() > 0.5f;
        return all ? kAllPadsMask : (1u << selectedPad);
    }

    std::unique_ptr<MutationFrame> Mutator::capture (uint32_t laneMask, uint32_t sliceMask) const
    {
        auto f = std::make_unique<MutationFrame>();
        auto& seq = processor.getBridge().seq;
        auto& sl  = processor.getBridge().sliceSeq;
        f->laneMask = laneMask;
        f->sliceMask = sliceMask;
        for (int p = 0; p < kNumPads; ++p)
        {
            if ((laneMask >> p) & 1u)
                for (int k = 0; k < SequencerData::kSteps; ++k) f->seq[(size_t) p][(size_t) k] = seq.getPacked (p, k);
            if ((sliceMask >> p) & 1u)
            {
                const int slot = processor.sliceSlotOf (p);
                f->sliceSlot[(size_t) p] = slot;
                f->slotWord[(size_t) p] = sl.getPackedSlot (p, slot);
                for (int k = 0; k < SliceSeqData::kSteps; ++k) f->sliceSteps[(size_t) p][(size_t) k] = sl.getPackedStep (p, slot, k);
            }
        }
        return f;
    }

    void Mutator::refreshOriginal()
    {
        // Slice sequences mutate as steps: turn an untouched slot (the break as
        // recorded) into its steps first. Inaudible.
        const uint32_t slicePads = sliceSeqPads();
        for (int p = 0; p < kNumPads; ++p)
            if ((slicePads >> p) & 1u) processor.materializeSliceSlot (p, processor.sliceSlotOf (p));

        auto& bridge = processor.getBridge();
        bool edited = original == nullptr || bridge.seq.getVersion() != seenSeqVersion || bridge.sliceSeq.getVersion() != seenSliceVersion
                      || original->sliceMask != slicePads;
        for (int p = 0; p < kNumPads && ! edited; ++p)
            edited = ((slicePads >> p) & 1u) && original->sliceSlot[(size_t) p] != processor.sliceSlotOf (p);

        if (edited)
        {
            // Anything changed by hand (edits, recording, snapshot recall, slot
            // change) since our last mutation: that is the new original.
            original = capture (kAllPadsMask, slicePads);
            undoFrame.reset();
            seenSeqVersion = bridge.seq.getVersion();
            seenSliceVersion = bridge.sliceSeq.getVersion();
        }
    }

    //==========================================================================
    //  Actions
    //==========================================================================
    void Mutator::mutate (int quant, int bars)
    {
        auto& apvts = processor.getParameters();
        if (quant < 0) quant = juce::roundToInt (apvts.getRawParameterValue (params::mutAt)->load());

        refreshOriginal();
        const bool drift = apvts.getRawParameterValue (params::mutDrift)->load() > 0.5f;
        const float amount = apvts.getRawParameterValue (params::mutAmount)->load() / 100.0f;
        uint32_t targets = 0;
        for (int t = 0; t < params::kNumMutTargets; ++t)
            if (apvts.getRawParameterValue (params::mutTargetId (t))->load() > 0.5f) targets |= 1u << t;

        const uint32_t scope = padsInScope();
        auto base = drift ? capture (kAllPadsMask, original->sliceMask) : std::make_unique<MutationFrame> (*original);
        auto f = std::make_unique<MutationFrame>();
        f->quant = juce::jlimit (0, 2, quant);
        f->bars  = juce::jmax (1, bars);

        const int seqLen = juce::jlimit (1, 32, juce::roundToInt (apvts.getRawParameterValue (params::seqLength)->load()));
        const int spb = stepsPerBeatMain();
        for (int p = 0; p < kNumPads; ++p)
        {
            if (((scope >> p) & 1u) == 0) continue;
            const auto info = processor.getSamples().getInfo (p);
            const int sliceCount = info.buffer == nullptr ? 0 : (info.slices.isEmpty() ? 16 : info.slices.count);

            // Main sequencer lane.
            Lane o, m;
            bool any = false;
            for (int k = 0; k < SequencerData::kSteps; ++k)
            {
                o[(size_t) k] = SeqStep::unpack (base->seq[(size_t) p][(size_t) k]);
                any = any || (k < seqLen && o[(size_t) k].on);
            }
            if (any)
            {
                mutateLane (m, o, seqLen, spb, sliceCount, amount, targets, rng);
                f->laneMask |= 1u << p;
                for (int k = 0; k < SequencerData::kSteps; ++k) f->seq[(size_t) p][(size_t) k] = m[(size_t) k].pack();
            }

            // Slice sequence (its current slot).
            if ((base->sliceMask >> p) & 1u)
            {
                const auto slot = SliceSlot::unpack (base->slotWord[(size_t) p]);
                Slots so, sm;
                for (int k = 0; k < SliceSeqData::kSteps; ++k) so[(size_t) k] = SliceStep::unpack (base->sliceSteps[(size_t) p][(size_t) k]);
                const int sspb = slot.rate == 3 ? 6 : juce::roundToInt (1.0 / kSliceRateBeats[slot.rate]);
                mutateSlices (sm, so, slot.length, sspb, sliceCount, amount, targets, rng);
                f->sliceMask |= 1u << p;
                f->sliceSlot[(size_t) p] = base->sliceSlot[(size_t) p];
                f->slotWord[(size_t) p] = base->slotWord[(size_t) p];
                for (int k = 0; k < SliceSeqData::kSteps; ++k) f->sliceSteps[(size_t) p][(size_t) k] = sm[(size_t) k].pack();
            }
        }
        if (f->laneMask == 0 && f->sliceMask == 0) return;

        // UNDO goes back to what this replaces.
        undoFrame = capture (f->laneMask, f->sliceMask);
        for (int p = 0; p < kNumPads; ++p) undoFrame->sliceSlot[(size_t) p] = f->sliceSlot[(size_t) p];
        if (f->sliceMask != 0)
            for (int p = 0; p < kNumPads; ++p)
                if ((f->sliceMask >> p) & 1u)
                {
                    auto& sl = processor.getBridge().sliceSeq;
                    const int slot = f->sliceSlot[(size_t) p];
                    undoFrame->slotWord[(size_t) p] = sl.getPackedSlot (p, slot);
                    for (int k = 0; k < SliceSeqData::kSteps; ++k) undoFrame->sliceSteps[(size_t) p][(size_t) k] = sl.getPackedStep (p, slot, k);
                }
        arm (std::move (f));
    }

    void Mutator::undo()
    {
        if (undoFrame == nullptr) return;
        auto f = std::move (undoFrame);
        undoFrame = capture (f->laneMask, f->sliceMask);        // UNDO again = redo
        f->quant = params::switchNow;
        arm (std::move (f));
    }

    void Mutator::reset()
    {
        if (original == nullptr) return;
        undoFrame = capture (original->laneMask, original->sliceMask);
        auto f = std::make_unique<MutationFrame> (*original);
        f->quant = params::switchNow;
        arm (std::move (f));
    }

    void Mutator::arm (std::unique_ptr<MutationFrame> f)
    {
        waiting = true;
        if (auto* old = processor.armMutationFrame (f.release()))
        {
            old->retireEpoch = processor.getBlockEpoch();
            retiring.emplace_back (old);
        }
    }

    //==========================================================================
    void Mutator::poll()
    {
        auto& bridge = processor.getBridge();
        if (auto* f = processor.takeAppliedMutationFrame())
        {
            waiting = processor.hasArmedMutation();
            ++count;
            seenSeqVersion = bridge.seq.getVersion();
            seenSliceVersion = bridge.sliceSeq.getVersion();
            processor.markStateChanged();
            f->retireEpoch = processor.getBlockEpoch();
            bool listed = false;
            for (auto& r : retiring) listed = listed || r.get() == f;
            if (! listed) retiring.emplace_back (f);
        }

        const auto epoch = processor.getBlockEpoch();

        // No audio running (track off, plugin idle): apply it here instead.
        if (waiting && processor.hasArmedMutation())
        {
            stalledPolls = epoch == lastEpoch ? stalledPolls + 1 : 0;
            if (stalledPolls > 8) { processor.applyMutationNow(); stalledPolls = 0; }
        }
        lastEpoch = epoch;

        for (size_t i = 0; i < retiring.size();)
        {
            auto* f = retiring[i].get();
            if (epoch >= f->retireEpoch + 2 && ! processor.isMutationFrameInUse (f))
                retiring.erase (retiring.begin() + (std::ptrdiff_t) i);
            else
                ++i;
        }

        auto& apvts = processor.getParameters();

        // "Mutate" parameter: a MIDI-mapped pad or automation, on its rising edge.
        const bool go = apvts.getRawParameterValue (params::mutGo)->load() > 0.5f;
        if (go && ! lastGo) mutate();
        lastGo = go;

        // AUTO: during the last beat of every N bars, arm a mutation for the bar line.
        const int autoIdx = juce::jlimit (0, 4, juce::roundToInt (apvts.getRawParameterValue (params::mutAuto)->load()));
        const auto& tr = bridge.transport;
        if (autoIdx == 0 || ! tr.isPlaying.load())
        {
            lastAutoKey = -1;
            return;
        }
        const int N    = params::mutAutoBars[autoIdx];
        const int bar  = tr.bar.load(), beat = tr.beat.load();
        const int num  = juce::jmax (1, tr.timeSigNum.load());
        const double ppq = tr.ppq.load();
        const int key = (bar / N + 1) * N;                       // the bar line it lands on
        if (ppq < lastAutoPpq - 0.5) lastAutoKey = -1;           // loop brace / relocate: that bar line comes round again
        if (bar % N == N - 1 && beat == num - 1 && key != lastAutoKey && ! waiting)
        {
            lastAutoKey = key;
            mutate (params::switchBar, N);
        }
        lastAutoPpq = ppq;
    }
}
