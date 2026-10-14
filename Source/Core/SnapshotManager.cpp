#include "Core/SnapshotManager.h"
#include "PluginProcessor.h"
#include <map>
#include <set>

namespace tb
{
    namespace
    {
        const juce::Identifier kParamId { "PARAM" };
        const juce::Identifier kIdId    { "id" };
        const juce::Identifier kValueId { "value" };
        const juce::Identifier kSlotId  { "slot" };
    }

    SnapshotManager::SnapshotManager (TwistedBreaksProcessor& p) : processor (p)
    {
        startTimerHz (30);
    }

    SnapshotManager::~SnapshotManager()
    {
        stopTimer();
    }

    bool SnapshotManager::isExcluded (const juce::String& id)
    {
        return id.startsWith ("master_") || id == params::snapRecall || id == params::snapAt
            || id == params::bankActive || id == params::bankAt || id == params::seqFill || id == params::mutGo
            || (id.startsWith ("macro_") && id != params::macroAmount && id != params::macroGroup && id != params::macroLatch);   // macros are moves, not state
    }

    //==========================================================================
    void SnapshotManager::store (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        juce::ValueTree t ("SNAPSHOT");
        t.setProperty (kSlotId, slot, nullptr);
        for (auto* prm : processor.getParameters().processor.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
                if (! isExcluded (r->getParameterID()))
                {
                    juce::ValueTree v (kParamId);
                    v.setProperty (kIdId, r->getParameterID(), nullptr);
                    v.setProperty (kValueId, r->convertFrom0to1 (r->getValue()), nullptr);
                    t.appendChild (v, nullptr);
                }
        t.appendChild (processor.getSamples().toValueTree(), nullptr);
        t.appendChild (processor.getBridge().seq.toValueTree(), nullptr);
        t.appendChild (processor.getBridge().sliceSeq.toValueTree(), nullptr);
        slots[(size_t) slot] = t;
        active = slot;
        ++version;
        processor.markStateChanged();
    }

    void SnapshotManager::clear (int slot)
    {
        if (! juce::isPositiveAndBelow (slot, kSlots)) return;
        slots[(size_t) slot] = {};
        if (active == slot) active = -1;
        ++version;
        processor.markStateChanged();
    }

    SnapshotManager::SlotState SnapshotManager::getState (int slot) const
    {
        if (! juce::isPositiveAndBelow (slot, kSlots) || ! slots[(size_t) slot].isValid()) return SlotState::empty;
        if (slot == loadingSlot) return SlotState::loading;
        if (slot == waitingSlot) return SlotState::waiting;
        if (slot == active)      return SlotState::active;
        return SlotState::stored;
    }

    //==========================================================================
    void SnapshotManager::recall (int slot, int quant)
    {
        if (! juce::isPositiveAndBelow (slot, kSlots) || ! slots[(size_t) slot].isValid()) return;
        if (quant < 0)
            quant = juce::roundToInt (processor.getParameters().getRawParameterValue (params::snapAt)->load());

        // Keep the "Snapshot" parameter in step (so recording automation works),
        // without it echoing back as a second request.
        if (auto* prm = processor.getParameters().getParameter (params::snapRecall))
        {
            lastParamValue = slot + 1;
            if (juce::roundToInt (prm->convertFrom0to1 (prm->getValue())) != slot + 1)
            {
                prm->beginChangeGesture();
                prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (slot + 1)));
                prm->endChangeGesture();
            }
        }

        // 1. Decode whatever this snapshot plays (shared with anything loaded).
        const auto gen = ++requestGen;
        loadingSlot = slot;
        ++version;

        auto pads = slots[(size_t) slot].getChildWithName (SampleManager::padsId);
        struct Pending { std::array<SampleBuffer::Ptr, kNumPads> buffers; int outstanding = 0; };
        auto state = std::make_shared<Pending>();
        std::vector<std::pair<int, juce::File>> wanted;
        for (const auto& e : pads)
        {
            const int p = (int) e.getProperty ("index", -1);
            const auto path = e.getProperty ("path").toString();
            if (juce::isPositiveAndBelow (p, kNumPads) && path.isNotEmpty())
                wanted.push_back ({ p, juce::File (path) });
        }
        state->outstanding = (int) wanted.size() + 1;

        juce::WeakReference<SnapshotManager> weak (this);
        auto finished = [weak, gen, slot, quant, state]
        {
            auto* self = weak.get();
            if (self == nullptr || gen != self->requestGen || --state->outstanding > 0) return;
            // 2. Everything decoded: build the frame and arm it.
            auto frame = self->buildFrame (slot, quant, state->buffers);
            self->loadingSlot = -1;
            self->waitingSlot = slot;
            ++self->version;
            if (auto* old = self->processor.armSnapshotFrame (frame.release()))
            {
                old->retireEpoch = self->processor.getBlockEpoch();
                self->retiring.emplace_back (old);
            }
        };

        for (const auto& [p, file] : wanted)
        {
            const int pad = p;
            processor.getSamples().requestBuffer (file, [state, pad, finished] (SampleBuffer::Ptr buf)
            {
                state->buffers[(size_t) pad] = buf;
                finished();
            });
        }
        finished();      // the "+1": fires now if nothing had to load
    }

    std::unique_ptr<SnapshotFrame> SnapshotManager::buildFrame (int slot, int quant, const std::array<SampleBuffer::Ptr, kNumPads>& buffers)
    {
        auto f = std::make_unique<SnapshotFrame>();
        f->slot  = slot;
        f->quant = juce::jlimit (0, 2, quant);
        const auto& t = slots[(size_t) slot];
        auto& apvts = processor.getParameters();

        // Parameters: the snapshot's value, or (for anything newer than the
        // snapshot) the current one.
        std::map<juce::String, float> stored;
        for (const auto& c : t)
            if (c.hasType (kParamId)) stored[c.getProperty (kIdId).toString()] = (float) c.getProperty (kValueId);

        std::map<const std::atomic<float>*, float> byAtomic;
        for (auto* prm : apvts.processor.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm))
            {
                const auto id = r->getParameterID();
                if (isExcluded (id)) continue;
                const auto it = stored.find (id);
                const float plain = it != stored.end() ? it->second : r->convertFrom0to1 (r->getValue());
                f->values.push_back ({ r, r->convertTo0to1 (plain) });
                byAtomic[apvts.getRawParameterValue (id)] = r->convertFrom0to1 (r->convertTo0to1 (plain));
            }
        processor.fillParamsFrom (f->params, [&byAtomic] (const std::atomic<float>* a)
        {
            const auto it = byAtomic.find (a);
            return it != byAtomic.end() ? it->second : a->load();
        });

        // Pads: the decoded buffers + their markers.
        f->pads = t.getChildWithName (SampleManager::padsId).createCopy();
        for (int p = 0; p < kNumPads; ++p)
        {
            f->buffers[(size_t) p] = buffers[(size_t) p];
            f->raw[(size_t) p] = buffers[(size_t) p].get();
        }
        for (const auto& e : f->pads)
        {
            const int p = (int) e.getProperty ("index", -1);
            if (! juce::isPositiveAndBelow (p, kNumPads) || buffers[(size_t) p] == nullptr) continue;
            auto s = SliceTable::fromString (e.getProperty ("slices").toString());
            s.normalise (buffers[(size_t) p]->getNumSamples());
            f->slices[(size_t) p] = s;
        }

        // Sequencers, as packed numbers.
        auto seq = std::make_unique<SequencerData>();
        seq->fromValueTree (t.getChildWithName (SequencerData::seqId));
        auto sl = std::make_unique<SliceSeqData>();
        sl->fromValueTree (t.getChildWithName (SliceSeqData::sliceSeqId));
        for (int p = 0; p < kNumPads; ++p)
        {
            for (int k = 0; k < SequencerData::kSteps; ++k) f->seq[(size_t) p][(size_t) k] = seq->getPacked (p, k);
            for (int s = 0; s < SliceSeqData::kSlots; ++s)
            {
                f->sliceSlots[(size_t) p][(size_t) s] = sl->getPackedSlot (p, s);
                for (int k = 0; k < SliceSeqData::kSteps; ++k) f->sliceSteps[(size_t) p][(size_t) s][(size_t) k] = sl->getPackedStep (p, s, k);
            }
        }
        return f;
    }

    //==========================================================================
    void SnapshotManager::poll()
    {
        // 3 -> 4. The audio thread switched: make the parameters / pads match.
        if (auto* f = processor.takeAppliedSnapshotFrame())
        {
            for (const auto& [prm, norm] : f->values)
                if (std::abs (prm->getValue() - norm) > 1.0e-7f)
                    prm->setValueNotifyingHost (norm);
            processor.getSamples().adoptPads (f->pads, f->buffers);
            processor.releaseSnapshotOverride (f);
            active = f->slot;
            if (waitingSlot == f->slot) waitingSlot = -1;
            ++version;
            processor.markStateChanged();
            f->retireEpoch = processor.getBlockEpoch();
            bool listed = false;
            for (auto& r : retiring) listed = listed || r.get() == f;
            if (! listed) retiring.emplace_back (f);
        }

        // Frames are deleted only once the audio thread has moved on.
        const auto epoch = processor.getBlockEpoch();
        for (size_t i = 0; i < retiring.size();)
        {
            auto* f = retiring[i].get();
            if (epoch >= f->retireEpoch + 2 && ! processor.isSnapshotFrameInUse (f))
                retiring.erase (retiring.begin() + (std::ptrdiff_t) i);
            else
                ++i;
        }

        // MIDI Program Change 1-8 / the automatable "Snapshot" parameter.
        const int pc = processor.getBridge().programChange.exchange (0);
        if (pc > 0) recall ((pc - 1) % kSlots);
        const int pv = juce::roundToInt (processor.getParameters().getRawParameterValue (params::snapRecall)->load());
        if (pv != lastParamValue)
        {
            lastParamValue = pv;
            if (pv > 0) recall (pv - 1);
        }
    }

    //==========================================================================
    juce::ValueTree SnapshotManager::toValueTree() const
    {
        juce::ValueTree t (snapshotsId);
        t.setProperty ("active", active, nullptr);
        for (const auto& s : slots)
            if (s.isValid()) t.appendChild (s.createCopy(), nullptr);
        return t;
    }

    void SnapshotManager::fromValueTree (const juce::ValueTree& t)
    {
        for (auto& s : slots) s = {};
        for (const auto& c : t)
        {
            const int i = (int) c.getProperty (kSlotId, -1);
            if (juce::isPositiveAndBelow (i, kSlots)) slots[(size_t) i] = c.createCopy();
        }
        active = (int) t.getProperty ("active", -1);
        lastParamValue = juce::roundToInt (processor.getParameters().getRawParameterValue (params::snapRecall)->load());
        ++version;
    }
}
