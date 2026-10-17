#include "Core/PadOps.h"
#include "Core/Parameters.h"
#include "PluginProcessor.h"

namespace tb
{
    namespace
    {
        juce::String prefixOf (int pad) { return params::padId (pad, ""); }    // "pad01_"

        bool keptOut (const juce::String& suffix)
        {
            return suffix == params::padMute || suffix == params::padSolo;
        }

        bool sameSample (const SampleManager::PadInfo& a, const SampleManager::PadInfo& b)
        {
            if (a.status != b.status) return false;
            if (a.status == SampleManager::Status::empty) return true;
            return a.file == b.file && a.buffer.get() == b.buffer.get()
                && a.slices == b.slices && a.detected == b.detected
                && a.sensitivity == b.sensitivity && a.sliceCount == b.sliceCount;
        }
    }

    bool PadOps::Content::sameAs (const Content& o, uint32_t p) const
    {
        if ((p & sample) != 0 && ! sameSample (info, o.info)) return false;
        if ((p & settings) != 0 && params != o.params) return false;
        if ((p & steps) != 0 && (lane != o.lane || slots != o.slots || sliceSteps != o.sliceSteps)) return false;
        return true;
    }

    PadOps::PadOps (TwistedBreaksProcessor& p) : processor (p)
    {
        startTimer (150);
    }

    PadOps::~PadOps() { stopTimer(); }

    //==========================================================================
    PadOps::Content PadOps::capture (int pad, uint32_t parts) const
    {
        Content c;
        c.parts = parts;
        if (! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return c;

        if ((parts & sample) != 0)
            c.info = processor.getSamples().getInfo (pad);

        if ((parts & settings) != 0)
        {
            const auto prefix = prefixOf (pad);
            for (auto* ap : processor.getParameters().processor.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (ap))
                {
                    const auto id = rp->getParameterID();
                    if (! id.startsWith (prefix)) continue;
                    const auto suffix = id.substring (prefix.length());
                    if (keptOut (suffix)) continue;
                    c.params.emplace_back (suffix, rp->getValue());
                }
        }

        if ((parts & steps) != 0)
        {
            auto& seq = processor.getBridge().seq;
            auto& ss  = processor.getBridge().sliceSeq;
            for (int k = 0; k < SequencerData::kSteps; ++k) c.lane[(size_t) k] = seq.getPacked (pad, k);
            for (int s = 0; s < SliceSeqData::kSlots; ++s)
            {
                c.slots[(size_t) s] = ss.getPackedSlot (pad, s);
                for (int k = 0; k < SliceSeqData::kSteps; ++k)
                    c.sliceSteps[(size_t) s][(size_t) k] = ss.getPackedStep (pad, s, k);
            }
        }
        return c;
    }

    void PadOps::apply (int pad, const Content& c, uint32_t parts)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return;
        parts &= c.parts;

        if ((parts & sample) != 0)
            processor.getSamples().assignPad (pad, c.info);

        if ((parts & settings) != 0)
        {
            auto& apvts = processor.getParameters();
            for (const auto& [suffix, value] : c.params)
                if (auto* rp = apvts.getParameter (params::padId (pad, suffix.toRawUTF8())))
                    if (rp->getValue() != value)
                    {
                        rp->beginChangeGesture();
                        rp->setValueNotifyingHost (value);
                        rp->endChangeGesture();
                    }
        }

        if ((parts & steps) != 0)
        {
            auto& seq = processor.getBridge().seq;
            auto& ss  = processor.getBridge().sliceSeq;
            for (int k = 0; k < SequencerData::kSteps; ++k) seq.setPacked (pad, k, c.lane[(size_t) k]);
            seq.bumpVersion();
            for (int s = 0; s < SliceSeqData::kSlots; ++s)
            {
                ss.setPackedSlot (pad, s, c.slots[(size_t) s]);
                for (int k = 0; k < SliceSeqData::kSteps; ++k)
                    ss.setPackedStep (pad, s, k, c.sliceSteps[(size_t) s][(size_t) k]);
            }
            ss.bumpVersion();
        }
        processor.markStateChanged();
    }

    //==========================================================================
    void PadOps::copy (int pad)
    {
        if (! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return;
        clip = capture (pad, all);
        clipFrom = pad;
        ++version;
    }

    bool PadOps::paste (int pad, uint32_t parts)
    {
        if (! hasClip() || ! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return false;
        PadChange ch { pad, capture (pad, parts), {} };
        apply (pad, clip, parts);
        ch.after = clip;
        ch.after.parts = parts;
        const char* what = parts == all ? "PASTE" : parts == sound ? "PASTE SOUND" : parts == steps ? "PASTE STEPS" : "PASTE SETTINGS";
        record (what, { std::move (ch) });
        return true;
    }

    void PadOps::copyPad (int from, int to, uint32_t parts)
    {
        if (from == to || ! juce::isPositiveAndBelow (from, SequencerData::kPads) || ! juce::isPositiveAndBelow (to, SequencerData::kPads)) return;
        auto src = capture (from, parts);
        PadChange ch { to, capture (to, parts), src };
        apply (to, src, parts);
        record ("COPY PAD", { std::move (ch) });
    }

    void PadOps::swap (int a, int b)
    {
        if (a == b || ! juce::isPositiveAndBelow (a, SequencerData::kPads) || ! juce::isPositiveAndBelow (b, SequencerData::kPads)) return;
        auto ca = capture (a, all), cb = capture (b, all);
        apply (a, cb, all);
        apply (b, ca, all);
        record ("SWAP PADS", { PadChange { a, ca, cb }, PadChange { b, cb, ca } });
    }

    void PadOps::clear (int pad)
    {
        if (! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return;
        auto before = capture (pad, sample | steps);
        Content empty;
        empty.parts = sample | steps;
        apply (pad, empty, sample | steps);
        record ("CLEAR PAD", { PadChange { pad, std::move (before), std::move (empty) } });
    }

    //==========================================================================
    void PadOps::record (const juce::String& name, std::vector<PadChange> changes)
    {
        if (changes.empty()) return;
        history.erase (history.begin() + index, history.end());       // a new edit drops the redo branch
        history.push_back ({ name, std::move (changes) });
        while ((int) history.size() > kMaxHistory) history.erase (history.begin());
        index = (int) history.size();
        settle (history.back());
        ++version;
    }

    void PadOps::settle (const Transaction& t)
    {
        for (const auto& c : t.changes) rebase (c.pad);
        lastStamp = watchStamp();
        dirty = false;
        if (processor.getSamples().isBusy()) needsRebase = true;     // files still loading: take them as they land
    }

    bool PadOps::undo()
    {
        if (! canUndo()) return false;
        const auto& t = history[(size_t) --index];
        for (auto it = t.changes.rbegin(); it != t.changes.rend(); ++it)
            apply (it->pad, it->before, it->before.parts);
        settle (t);
        ++version;
        return true;
    }

    bool PadOps::redo()
    {
        if (! canRedo()) return false;
        const auto& t = history[(size_t) index++];
        for (const auto& c : t.changes)
            apply (c.pad, c.after, c.after.parts);
        settle (t);
        ++version;
        return true;
    }

    void PadOps::beginEdit()
    {
        poll (lastChangeMs + kQuietMs * 10.0);            // settle anything pending first, as its own step
        editBefore.clear();
        for (int p = 0; p < SequencerData::kPads; ++p) editBefore.push_back (capture (p, all));
        editing = true;
    }

    void PadOps::endEdit (const juce::String& name, uint32_t parts)
    {
        if (! editing) return;
        editing = false;
        std::vector<PadChange> changes;
        for (int p = 0; p < SequencerData::kPads; ++p)
        {
            auto now = capture (p, parts);
            auto before = editBefore[(size_t) p];
            before.parts = parts;
            if (! before.sameAs (now, parts))
            {
                // A sample still loading is recorded as its file (it reloads on undo / redo).
                changes.push_back ({ p, std::move (before), std::move (now) });
            }
        }
        editBefore.clear();
        if (changes.empty()) return;
        record (name, std::move (changes));
    }

    void PadOps::reset()
    {
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm == nullptr || ! mm->isThisTheMessageThread())
        {
            juce::WeakReference<PadOps> weak (this);
            juce::MessageManager::callAsync ([weak] { if (auto* self = weak.get()) self->reset(); });
            return;
        }
        history.clear();
        index = 0;
        editing = false;
        editBefore.clear();
        needsRebase = true;
        dirty = false;
        ++version;
    }

    //==========================================================================
    uint64_t PadOps::watchStamp() const
    {
        uint64_t s = (uint64_t) processor.getBridge().seq.getVersion() * 1000003ull
                   + (uint64_t) processor.getBridge().sliceSeq.getVersion() * 7919ull;
        for (int p = 0; p < SequencerData::kPads; ++p)
            s = s * 31ull + processor.getSamples().getVersion (p);
        return s;
    }

    void PadOps::rebase (int pad)
    {
        baseline[(size_t) pad] = capture (pad, kWatched);
    }

    void PadOps::rebaseAll()
    {
        for (int p = 0; p < SequencerData::kPads; ++p) rebase (p);
        lastStamp = watchStamp();
        dirty = false;
    }

    void PadOps::poll (double nowMs)
    {
        if (editing) return;
        const auto stamp = watchStamp();
        if (stamp != lastStamp)
        {
            lastStamp = stamp;
            lastChangeMs = nowMs;
            dirty = true;
            return;
        }
        if (! dirty && ! needsRebase) return;
        if (nowMs - lastChangeMs < kQuietMs || processor.getSamples().isBusy()) return;

        if (needsRebase)
        {
            needsRebase = false;
            rebaseAll();
            return;
        }

        dirty = false;
        std::vector<PadChange> changes;
        for (int p = 0; p < SequencerData::kPads; ++p)
        {
            auto now = capture (p, kWatched);
            if (! baseline[(size_t) p].sameAs (now, kWatched))
            {
                // Only the part that changed goes in, so undo leaves the other alone.
                uint32_t parts = 0;
                if (! baseline[(size_t) p].sameAs (now, sample)) parts |= sample;
                if (! baseline[(size_t) p].sameAs (now, steps))  parts |= steps;
                auto before = baseline[(size_t) p];
                before.parts = parts;
                now.parts = parts;
                changes.push_back ({ p, std::move (before), std::move (now) });
            }
        }
        if (changes.empty()) return;

        juce::String name = "EDIT";
        bool allSteps = true, allSample = true;
        for (const auto& c : changes) { allSteps = allSteps && c.after.parts == steps; allSample = allSample && c.after.parts == sample; }
        if (allSteps)  name = "STEP EDIT";
        if (allSample) name = changes.size() == 1 && changes.front().after.info.buffer.get() == changes.front().before.info.buffer.get() ? "SLICE EDIT" : "SAMPLE";
        record (name, std::move (changes));
    }
}
