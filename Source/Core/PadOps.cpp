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
            for (int l = 0; l < SampleManager::PadInfo::kLayers; ++l)
            {
                const auto& x = a.layers[(size_t) l];
                const auto& y = b.layers[(size_t) l];
                if (x.status != y.status || x.file != y.file || x.buffer.get() != y.buffer.get()) return false;
            }
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

    void PadOps::timerCallback()
    {
        poll (juce::Time::getMillisecondCounterHiRes());
        // The "Twist" parameter (MIDI-mappable): a rising edge twists the selected pad.
        if (auto* v = processor.getParameters().getRawParameterValue (params::twistGo))
        {
            const bool on = v->load() > 0.5f;
            if (on && ! lastTwistGo) twist (selected);
            lastTwistGo = on;
        }
    }

    void PadOps::twist (int pad)
    {
        if (! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return;
        auto& apvts = processor.getParameters();
        const float amt = juce::jlimit (0.0f, 1.0f, apvts.getRawParameterValue (params::twistAmount)->load() / 100.0f);
        auto locked = [&apvts] (int t) { return apvts.getRawParameterValue (params::twistLockId (t))->load() > 0.5f; };

        // Twist around where you left it: a new base unless this is still the last twist's result.
        auto now = capture (pad, settings);
        if (twistResult[(size_t) pad].parts == 0 || ! now.sameAs (twistResult[(size_t) pad], settings))
            twistBase[(size_t) pad] = now;
        const auto& base = twistBase[(size_t) pad];

        auto prm = [&apvts, pad] (const char* name) { return apvts.getParameter (params::padId (pad, name)); };
        auto baseOf = [&] (const char* name) -> float
        {
            auto* r = prm (name);
            for (const auto& [k, v] : base.params) if (k == name) return r->convertFrom0to1 (v);
            return r->convertFrom0to1 (r->getValue());
        };
        auto set = [&] (const char* name, float plain)
        {
            if (auto* r = prm (name))
            {
                const float norm = r->convertTo0to1 (plain);
                if (std::abs (norm - r->getValue()) < 1.0e-6f) return;
                r->beginChangeGesture();
                r->setValueNotifyingHost (norm);
                r->endChangeGesture();
            }
        };
        auto& rnd = twistRandom;
        auto bi = [&rnd] { return rnd.nextFloat() * 2.0f - 1.0f; };

        if (! locked (0))
        {
            const int range = juce::roundToInt (12.0f * amt);
            set (params::padPitch, juce::jlimit (-params::padPitchRange, params::padPitchRange,
                                                 baseOf (params::padPitch) + (float) (rnd.nextInt (2 * range + 1) - range)));
        }
        if (! locked (1))
        {
            const float end = baseOf (params::padEnd);
            set (params::padStart, juce::jlimit (0.0f, juce::jmax (0.0f, end - 0.1f), baseOf (params::padStart) + rnd.nextFloat() * 0.45f * amt));
        }
        if (! locked (2)) set (params::padPan, juce::jlimit (-1.0f, 1.0f, baseOf (params::padPan) + bi() * amt));
        if (! locked (3))
        {
            const bool rev = baseOf (params::padReverse) > 0.5f;
            set (params::padReverse, (rnd.nextFloat() < 0.35f * amt ? ! rev : rev) ? 1.0f : 0.0f);
        }
        if (! locked (4))
        {
            auto* r = prm (params::padCharacter);
            const int n = r != nullptr ? juce::jmax (1, r->getNumSteps()) : 1;
            set (params::padCharacter, rnd.nextFloat() < amt ? (float) rnd.nextInt (n) : baseOf (params::padCharacter));
        }
        if (! locked (5))
        {
            const int count = processor.getSamples().getInfo (pad).slices.count;
            if (count > 1) set (params::padSlice, rnd.nextFloat() < amt ? (float) (1 + rnd.nextInt (count)) : baseOf (params::padSlice));
        }
        if (! locked (6))
            set (params::padGain, juce::jlimit (params::padGainMinDb, params::padGainMaxDb, baseOf (params::padGain) + bi() * 4.0f * amt));

        auto after = capture (pad, settings);
        twistResult[(size_t) pad] = after;
        if (! after.sameAs (now, settings))
            record ("TWIST", { PadChange { pad, std::move (now), std::move (after) } });
    }

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

    juce::ValueTree PadOps::toTree (const Content& c)
    {
        juce::ValueTree t ("PAD");
        if ((c.parts & sample) != 0 && c.info.status != SampleManager::Status::empty)
        {
            juce::ValueTree s ("SAMPLE");
            s.setProperty ("path", c.info.file.getFullPathName(), nullptr);
            s.setProperty ("slices", c.info.slices.toString(), nullptr);
            s.setProperty ("detected", c.info.detected.toString(), nullptr);
            s.setProperty ("sensitivity", c.info.sensitivity, nullptr);
            s.setProperty ("sliceCount", c.info.sliceCount, nullptr);
            for (int l = 0; l < SampleManager::PadInfo::kLayers; ++l)
                if (c.info.layers[(size_t) l].status != SampleManager::Status::empty)
                {
                    juce::ValueTree n ("LAYER");
                    n.setProperty ("index", l, nullptr);
                    n.setProperty ("path", c.info.layers[(size_t) l].file.getFullPathName(), nullptr);
                    s.appendChild (n, nullptr);
                }
            t.appendChild (s, nullptr);
        }
        if ((c.parts & settings) != 0)
        {
            juce::ValueTree ps ("SETTINGS");
            for (const auto& [suffix, v] : c.params)
            {
                juce::ValueTree n ("PARAM");
                n.setProperty ("id", suffix, nullptr);
                n.setProperty ("value", v, nullptr);
                ps.appendChild (n, nullptr);
            }
            t.appendChild (ps, nullptr);
        }
        if ((c.parts & steps) != 0)
        {
            juce::ValueTree st ("STEPS");
            juce::StringArray lane;
            for (auto v : c.lane) lane.add (juce::String::toHexString ((juce::int64) v));
            st.setProperty ("lane", lane.joinIntoString (","), nullptr);
            for (int s = 0; s < SliceSeqData::kSlots; ++s)
            {
                juce::ValueTree n ("SLOT");
                n.setProperty ("index", s, nullptr);
                n.setProperty ("slot", juce::String::toHexString ((juce::int64) c.slots[(size_t) s]), nullptr);
                juce::StringArray v;
                for (auto x : c.sliceSteps[(size_t) s]) v.add (juce::String::toHexString ((juce::int64) x));
                n.setProperty ("steps", v.joinIntoString (","), nullptr);
                st.appendChild (n, nullptr);
            }
            t.appendChild (st, nullptr);
        }
        t.setProperty ("parts", (int) c.parts, nullptr);
        return t;
    }

    PadOps::Content PadOps::fromTree (const juce::ValueTree& t)
    {
        Content c;
        c.parts = (uint32_t) (int) t.getProperty ("parts", 0);
        auto hex = [] (const juce::String& s) { return (uint64_t) s.getHexValue64(); };
        if (auto s = t.getChildWithName ("SAMPLE"); s.isValid() && s.getProperty ("path").toString().isNotEmpty())
        {
            c.info.status = SampleManager::Status::loading;
            c.info.file = juce::File (s.getProperty ("path").toString());
            c.info.name = c.info.file.getFileNameWithoutExtension();
            c.info.slices = SliceTable::fromString (s.getProperty ("slices").toString());
            c.info.detected = SliceTable::fromString (s.getProperty ("detected").toString());
            c.info.sensitivity = (float) s.getProperty ("sensitivity", 0.5f);
            c.info.sliceCount = juce::jlimit (1, SliceTable::kMax, (int) s.getProperty ("sliceCount", 16));
            for (const auto& n : s)
            {
                const int l = (int) n.getProperty ("index", -1);
                if (! n.hasType ("LAYER") || ! juce::isPositiveAndBelow (l, SampleManager::PadInfo::kLayers)) continue;
                auto& L = c.info.layers[(size_t) l];
                L.status = SampleManager::Status::loading;
                L.file = juce::File (n.getProperty ("path").toString());
                L.name = L.file.getFileNameWithoutExtension();
            }
        }
        for (const auto& n : t.getChildWithName ("SETTINGS"))
            c.params.emplace_back (n.getProperty ("id").toString(), juce::jlimit (0.0f, 1.0f, (float) n.getProperty ("value", 0.0f)));
        if (auto st = t.getChildWithName ("STEPS"); st.isValid())
        {
            const auto lane = juce::StringArray::fromTokens (st.getProperty ("lane").toString(), ",", "");
            for (int k = 0; k < juce::jmin (SequencerData::kSteps, lane.size()); ++k) c.lane[(size_t) k] = hex (lane[k]);
            for (const auto& n : st)
            {
                const int s = (int) n.getProperty ("index", -1);
                if (! juce::isPositiveAndBelow (s, SliceSeqData::kSlots)) continue;
                c.slots[(size_t) s] = (uint32_t) hex (n.getProperty ("slot").toString());
                const auto v = juce::StringArray::fromTokens (n.getProperty ("steps").toString(), ",", "");
                for (int k = 0; k < juce::jmin (SliceSeqData::kSteps, v.size()); ++k) c.sliceSteps[(size_t) s][(size_t) k] = hex (v[k]);
            }
        }
        return c;
    }

    void PadOps::load (int pad, const Content& c, const juce::String& name)
    {
        if (! juce::isPositiveAndBelow (pad, SequencerData::kPads)) return;
        PadChange ch { pad, capture (pad, c.parts), c };
        apply (pad, c, c.parts);
        record (name, { std::move (ch) });
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
