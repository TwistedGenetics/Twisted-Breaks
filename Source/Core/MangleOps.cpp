#include "Core/MangleOps.h"
#include "PluginProcessor.h"
#include "Engine/Mangler.h"
#include <cstring>

namespace tb
{
    using namespace mangle;

    namespace
    {
        int rawInt (TwistedBreaksProcessor& p, const char* id)
        {
            return juce::roundToInt (p.getParameters().getRawParameterValue (id)->load());
        }
    }

    struct MangleOps::Job final : juce::Thread
    {
        Job (juce::WeakReference<MangleOps> o, int sl, Pattern b, int s, int st, bool m, juce::AudioBuffer<float> a, int len,
             double rate, double tempo, double bar, juce::int64 seed)
            : juce::Thread ("MANGLE randomize"), owner (o), slot (sl), base (b), steps (s), style (st), mutateOnly (m),
              audio (std::move (a)), length (len), sr (rate), bpm (tempo), ppqPerBar (bar), rng (seed) {}

        void run() override
        {
            int kept = 0;
            const auto result = MangleOps::choose (base, steps, style, mutateOnly, audio, length, sr, bpm, ppqPerBar, rng, kept);
            auto o = owner;
            const bool listened = length > 0;
            const int sl = slot;
            const bool mut = mutateOnly;
            juce::MessageManager::callAsync ([o, result, kept, listened, sl, mut]
            {
                if (auto* ops = o.get())
                {
                    ops->busy = false;
                    ops->lastReport = listened ? juce::String (kept) + " of 8 passed" : juce::String ("Live stopped: not checked");
                    ops->finish (sl, result, mut);
                }
            });
        }

        juce::WeakReference<MangleOps> owner;
        int slot;
        Pattern base;
        int steps, style;
        bool mutateOnly;
        juce::AudioBuffer<float> audio;
        int length;
        double sr, bpm, ppqPerBar;
        juce::Random rng;
    };

    MangleOps::MangleOps (TwistedBreaksProcessor& p) : processor (p) {}

    MangleOps::~MangleOps()
    {
        stopTimer();
        if (worker != nullptr) worker->stopThread (5000);
    }

    int MangleOps::slot() const { return juce::jlimit (0, mangle::kSlots - 1, rawInt (processor, params::mglSlot)); }
    Pattern MangleOps::get() const { return processor.getBridge().mangle.get (slot()); }
    Pattern MangleOps::get (int s) const { return processor.getBridge().mangle.get (s); }

    void MangleOps::set (const Pattern& p, bool recordHistory) { setSlot (slot(), p, recordHistory); }

    void MangleOps::setSlot (int s, const Pattern& p, bool recordHistory)
    {
        const auto before = get (s);
        if (recordHistory && before != p)
        {
            history.push_back ({ s, before });
            while (history.size() > 3) history.pop_front();
        }
        processor.getBridge().mangle.set (s, p);
        changed();
    }

    void MangleOps::copySlot (int from, int to)
    {
        if (from == to) return;
        setSlot (to, get (from), true);
    }

    void MangleOps::changed()
    {
        processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
        if (onChange) onChange();
    }

    int MangleOps::steps() const { return params::mglStepCounts[juce::jlimit (0, 2, rawInt (processor, params::mglSteps))]; }
    int MangleOps::style() const { return juce::jlimit (0, 2, rawInt (processor, params::mglStyle)); }

    void MangleOps::paint (int a, int b, int move, bool join)
    {
        auto p = get();
        p.paint (a, b, move, join);
        set (p);
    }
    void MangleOps::clearStep (int step)
    {
        auto p = get();
        const int s0 = p.runStart (step);
        p.split (s0);
        p.paint (step, step, thru, false);
        set (p);
    }
    void MangleOps::split (int step) { auto p = get(); p.split (step); set (p); }
    void MangleOps::setArg (int step, int arg)
    {
        auto p = get();
        const int s0 = p.runStart (juce::jlimit (0, Pattern::kSteps - 1, step));
        p.arg[(size_t) s0] = (uint8_t) juce::jlimit (0, 15, arg);
        set (p);
    }
    void MangleOps::nudgeArg (int step, int delta)
    {
        auto p = get();
        const int s0 = p.runStart (juce::jlimit (0, Pattern::kSteps - 1, step));
        const int range = p.move[(size_t) s0] == shuffle ? steps() : 3;
        p.arg[(size_t) s0] = (uint8_t) (((int) p.arg[(size_t) s0] + delta % range + range) % range);
        set (p);
    }

    bool MangleOps::revert()
    {
        if (history.empty()) return false;
        const auto h = history.back();
        history.pop_back();
        processor.getBridge().mangle.set (h.first, h.second);
        changed();
        return true;
    }

    //==============================================================================
    Pattern MangleOps::generate (const Pattern& base, int S, int st, bool mutateOnly, juce::Random& r)
    {
        S = juce::jlimit (1, Pattern::kSteps, S);
        static const uint8_t gentleMoves[] = { repeat, repeat, reverse };
        static const uint8_t mediumMoves[] = { repeat, repeat, reverse, half, dbl, stop, shuffle };
        static const uint8_t wildMoves[]   = { repeat, reverse, half, stop, dbl, scratch, freeze, shuffle };
        auto pickMove = [&]() -> uint8_t
        {
            if (st == gentle) return gentleMoves[r.nextInt (3)];
            if (st == medium) return mediumMoves[r.nextInt (7)];
            return wildMoves[r.nextInt (8)];
        };
        const int maxJoin = st == gentle ? (S >= 16 ? 2 : 1) : st == medium ? juce::jmax (1, S / 4) : juce::jmax (2, S / 2);
        auto pickArg = [&] (uint8_t m, int s) -> uint8_t
        {
            if (m == repeat)  return (uint8_t) (st == gentle ? r.nextInt (2) : r.nextInt (3));
            if (m == shuffle) { int src = r.nextInt (S); if (src == s) src = (s + 1 + r.nextInt (juce::jmax (1, S - 1))) % S; return (uint8_t) src; }
            return 0;
        };
        // Turnaround feel: later steps are likelier.
        auto pickStep = [&]() -> int
        {
            const int total = S * (S + 1) / 2;
            int k = r.nextInt (total);
            for (int s = 0; s < S; ++s) { k -= s + 1; if (k < 0) return s; }
            return S - 1;
        };

        Pattern p = mutateOnly ? base : Pattern{};
        p.trim = 1.0f;
        if (! mutateOnly)
        {
            const float share = st == gentle ? 0.25f : st == medium ? 0.45f : 0.65f;
            const int target = juce::jmax (1, juce::roundToInt (share * S));
            for (int guard = 0; p.activeSteps (S) < target && guard < 64; ++guard)
            {
                const int s = pickStep();
                if (p.move[(size_t) p.runStart (s)] != thru) continue;
                int len = 1 + (maxJoin > 1 ? r.nextInt (maxJoin) : 0);
                len = juce::jmin (len, S - s);
                for (int k = 1; k < len; ++k) if (p.move[(size_t) p.runStart (s + k)] != thru) { len = k; break; }
                const auto m = pickMove();
                p.paint (s, s + len - 1, m, len > 1);
                p.arg[(size_t) s] = pickArg (m, s);
            }
        }
        else
        {
            const int changes = 1 + r.nextInt (st == gentle ? 1 : st == medium ? 2 : 3);
            for (int c = 0; c < changes; ++c)
            {
                const int s = r.nextInt (S);
                const int s0 = p.runStart (s);
                const int what = r.nextInt (3);
                if (what == 0 && p.move[(size_t) s0] != thru)          { p.split (s0); p.paint (s, s, thru, false); }
                else if (what == 1 && (p.move[(size_t) s0] == repeat || p.move[(size_t) s0] == shuffle))
                    p.arg[(size_t) s0] = pickArg (p.move[(size_t) s0], s0);
                else
                {
                    const auto m = pickMove();
                    if (p.runLength (s0, S) > 1) { p.move[(size_t) s0] = m; p.arg[(size_t) s0] = pickArg (m, s0); }
                    else                         { p.paint (s, s, m, false); p.arg[(size_t) s] = pickArg (m, s); }
                }
            }
        }
        // Steps beyond S stay as they were (a later 16-step bar keeps them), but a run can't start past S.
        return p;
    }

    MangleOps::Check MangleOps::evaluate (const Pattern& p, int S, const juce::AudioBuffer<float>& in, int length,
                                          double sr, double bpm, double ppqPerBar)
    {
        Check c;
        if (length <= 16) { c.silentInput = true; return c; }
        // Twice round, judged on the second pass (so the bar before is there for REVERSE, SHUFFLE...).
        juce::AudioBuffer<float> twice (2, length * 2);
        for (int ch = 0; ch < 2; ++ch)
        {
            const int sc = juce::jmin (ch, in.getNumChannels() - 1);
            twice.copyFrom (ch, 0, in, sc, 0, length);
            twice.copyFrom (ch, length, in, sc, 0, length);
        }
        Pattern q = p;
        q.trim = 1.0f;
        juce::AudioBuffer<float> out;
        Mangler::renderOffline (q, S, twice, length * 2, sr, bpm, ppqPerBar, out);

        // Energy of the THRU steps and of the moved steps apart: trim only scales the moves.
        const double stepLen = (double) sr * 60.0 / bpm * ppqPerBar / (double) juce::jmax (1, S);
        auto isMove = [&] (int i)
        {
            const int step = (int) std::fmod ((double) i / stepLen, (double) S);
            return p.move[(size_t) p.runStart (juce::jlimit (0, S - 1, step))] != thru;
        };
        double dryE = 0.0, wetE = 0.0, thruE = 0.0, moveE = 0.0;
        float dryPeak = 0.0f, wetPeak = 0.0f, dryD2 = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* d = twice.getReadPointer (ch, length);
            const float* w = out.getReadPointer (ch, length);
            for (int i = 0; i < length; ++i)
            {
                dryE += (double) d[i] * d[i];
                wetE += (double) w[i] * w[i];
                (isMove (length + i) ? moveE : thruE) += (double) w[i] * w[i];
                dryPeak = juce::jmax (dryPeak, std::abs (d[i]));
                wetPeak = juce::jmax (wetPeak, std::abs (w[i]));
                if (i >= 2) dryD2 = juce::jmax (dryD2, std::abs (d[i] - 2.0f * d[i - 1] + d[i - 2]));
            }
        }
        const double n = 2.0 * length;
        const double dryRms = std::sqrt (dryE / n), wetRms = std::sqrt (wetE / n);
        if (dryRms < 1.0e-4) { c.silentInput = true; return c; }

        const float thr = dryD2 * 1.5f + 0.02f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* w = out.getReadPointer (ch, length);
            for (int i = 2; i < length; ++i)
                if (std::abs (w[i] - 2.0f * w[i - 1] + w[i - 2]) > thr) { ++c.clicks; i += 32; }
        }
        c.vanish  = wetRms < 0.3 * dryRms;
        c.runaway = wetPeak > dryPeak * 2.0f + 0.05f;
        const double want = dryE - thruE;
        c.trim    = (float) juce::jlimit (0.5, 2.0, (moveE > 1.0e-9 && want > 0.0) ? std::sqrt (want / moveE) : 1.0);
        return c;
    }

    Pattern MangleOps::choose (const Pattern& base, int S, int st, bool mutateOnly, const juce::AudioBuffer<float>& in,
                               int length, double sr, double bpm, double ppqPerBar, juce::Random& r, int& kept, int tries)
    {
        kept = 0;
        Pattern best = base;
        float bestScore = 1.0e9f;
        for (int t = 0; t < tries; ++t)
        {
            auto cand = generate (base, S, st, mutateOnly, r);
            if (length <= 0) { cand.trim = 1.0f; kept = 1; return cand; }
            const auto c = evaluate (cand, S, in, length, sr, bpm, ppqPerBar);
            if (c.silentInput) { cand.trim = 1.0f; kept = 1; return cand; }
            if (! c.ok()) continue;
            ++kept;
            if (c.score() < bestScore) { bestScore = c.score(); best = cand; best.trim = c.trim; }
        }
        return best;
    }

    void MangleOps::applyChosen (bool mutateOnly, const juce::AudioBuffer<float>& in, int length, double sr, double bpm, double ppqPerBar)
    {
        int kept = 0;
        const auto p = choose (get(), steps(), style(), mutateOnly, in, length, sr, bpm, ppqPerBar, rng, kept);
        lastReport = length > 0 ? juce::String (kept) + " of 8 passed" : juce::String ("Live stopped: not checked");
        finish (slot(), p, mutateOnly);
    }

    void MangleOps::finish (int s, const Pattern& p, bool mutateOnly)
    {
        setSlot (s, p, true);
        if (mutateOnly) return;
        // A new bar comes with its own TEXTURE / SPACE, unless they're locked.
        const bool lt = rawInt (processor, params::mglLockTex) != 0, ls = rawInt (processor, params::mglLockSpace) != 0;
        for (const auto& kv : randomSound (style(), lt, ls, rng)) setPlain (kv.first, kv.second);
    }

    void MangleOps::setPlain (const juce::String& id, float v)
    {
        if (auto* r = processor.getParameters().getParameter (id))
        {
            r->beginChangeGesture();
            r->setValueNotifyingHost (r->convertTo0to1 (v));
            r->endChangeGesture();
        }
    }

    //==============================================================================
    void MangleOps::start (bool mutateOnly)
    {
        if (busy) return;
        busy = true;
        pendingMutate = mutateOnly;
        auto& m = processor.getEngine().getMangler();
        captureSeen = m.captureCount();
        m.requestCapture (2);
        waitedMs = 0;
        startTimer (15);
    }

    void MangleOps::timerCallback()
    {
        auto& m = processor.getEngine().getMangler();
        waitedMs += 15;
        const bool got = m.captureReady (captureSeen);
        if (! got && waitedMs < 250) return;
        stopTimer();

        juce::AudioBuffer<float> audio;
        int len = 0;
        double bpm = 120.0, barSamples = 0.0;
        if (got)
        {
            const auto& cap = m.getCapture (len, bpm, barSamples);
            audio.makeCopyOf (cap);
            audio.setSize (2, juce::jmax (1, len), true, false, false);
        }
        const double sr = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        const double ppqPerBar = barSamples > 0.0 ? barSamples * bpm / 60.0 / sr : 4.0;
        if (worker != nullptr) worker->stopThread (2000);
        worker = std::make_unique<Job> (this, slot(), get(), steps(), style(), pendingMutate, std::move (audio), got ? len : 0,
                                        sr, bpm, ppqPerBar, rng.nextInt64());
        worker->startThread (juce::Thread::Priority::low);
    }

    //==============================================================================
    juce::ValueTree MangleOps::toValueTree() const
    {
        juce::ValueTree t (mangleId);
        for (int k = 0; k < mangle::kSlots; ++k)
        {
            auto c = get (k).toValueTree();
            c.setProperty ("slot", k, nullptr);
            t.appendChild (c, nullptr);
        }
        return t;
    }

    void MangleOps::fromValueTree (const juce::ValueTree& t)
    {
        history.clear();
        for (int k = 0; k < mangle::kSlots; ++k) processor.getBridge().mangle.set (k, Pattern{});
        if (t.isValid())
            for (const auto& c : t)
                if (c.hasType ("PATTERN"))
                    processor.getBridge().mangle.set ((int) c.getProperty ("slot", 0), Pattern::fromValueTree (c));   // 0.29 sets: one, slot A
        if (onChange) onChange();
    }

    //==============================================================================
    //  0.30: TEXTURE / SPACE for randomize, presets
    //==============================================================================
    std::vector<std::pair<juce::String, float>> MangleOps::randomSound (int st, bool lockTex, bool lockSpace, juce::Random& r)
    {
        std::vector<std::pair<juce::String, float>> v;
        auto chance = [&r] (float p) { return r.nextFloat() < p; };
        auto range = [&r] (float a, float b) { return (float) juce::roundToInt (a + (b - a) * r.nextFloat()); };
        if (! lockTex)
        {
            const float p = st == gentle ? 0.0f : st == medium ? 0.4f : 0.7f;
            if (chance (p))
            {
                const int mode = st == medium ? 1 + r.nextInt (2) : 1 + r.nextInt (4);
                v.push_back ({ params::mglTexMode, (float) mode });
                v.push_back ({ params::mglTexAmount, st == medium ? range (25, 55) : range (40, 90) });
                v.push_back ({ params::mglTexSize, st == medium ? range (20, 70) : range (15, 160) });
                v.push_back ({ params::mglTexDensity, range (8, st == medium ? 24.0f : 40.0f) });
                v.push_back ({ params::mglTexScatter, st == medium ? range (10, 45) : range (20, 90) });
            }
            else v.push_back ({ params::mglTexMode, 0.0f });
        }
        if (! lockSpace)
        {
            const float p = st == gentle ? 0.3f : st == medium ? 0.5f : 0.7f;
            if (chance (p))
            {
                const int mode = st == gentle ? 1 : st == medium ? 1 + r.nextInt (2) : 1 + r.nextInt (3);
                static const int gentleTimes[] = { 4, 6, 7 };              // 1/8, 1/8 D, 1/4
                v.push_back ({ params::mglSpaceMode, (float) mode });
                v.push_back ({ params::mglSpaceTime, (float) (st == wild ? r.nextInt (params::kMglSpaceTimes) : gentleTimes[r.nextInt (3)]) });
                v.push_back ({ params::mglSpaceFb, st == gentle ? range (20, 40) : st == medium ? range (30, 55) : range (35, 80) });
                v.push_back ({ params::mglSpaceAmount, st == gentle ? range (15, 30) : range (25, 50) });
            }
            else v.push_back ({ params::mglSpaceMode, 0.0f });
        }
        return v;
    }

    namespace
    {
        /** A bar from text: . THRU, r / R / 8 REPEAT x2 / x4 / x8, v REVERSE, h HALF, s STOP, d DOUBLE,
            x SCRATCH, f FREEZE, u SHUFFLE (the step four on), - joins the step to the one before. */
        Pattern bar (const char* text)
        {
            Pattern p;
            const int n = (int) std::strlen (text);
            for (int s = 0; s < n && s < Pattern::kSteps; ++s)
            {
                const char c = text[s];
                if (c == '-' && s > 0)
                {
                    p.tie[(size_t) s] = 1;
                    p.move[(size_t) s] = p.move[(size_t) s - 1];
                    p.arg[(size_t) s] = p.arg[(size_t) s - 1];
                    continue;
                }
                uint8_t m = thru, a = 0;
                switch (c)
                {
                    case 'r': m = repeat; a = 0; break;
                    case 'R': m = repeat; a = 1; break;
                    case '8': m = repeat; a = 2; break;
                    case 'v': m = reverse; break;
                    case 'h': m = half; break;
                    case 's': m = stop; break;
                    case 'd': m = dbl; break;
                    case 'x': m = scratch; break;
                    case 'f': m = freeze; break;
                    case 'u': m = shuffle; a = (uint8_t) ((s + 4) % n); break;
                    default: break;
                }
                p.move[(size_t) s] = m;
                p.arg[(size_t) s] = a;
            }
            return p;
        }

        int stepsIndex (const char* text) { const auto n = std::strlen (text); return n <= 4 ? 0 : n <= 8 ? 1 : 2; }
    }

    const std::vector<MangleOps::Preset>& MangleOps::factoryPresets()
    {
        static const std::vector<Preset> list = []
        {
            std::vector<Preset> v;
            auto add = [&v] (const char* group, const char* name, std::initializer_list<const char*> bars,
                             std::initializer_list<std::pair<const char*, float>> extra = {})
            {
                Preset p;
                p.group = group; p.name = name;
                int k = 0;
                for (auto* b : bars) if (k < mangle::kSlots) p.slots[(size_t) k++] = bar (b);
                p.params = { { params::mglSteps, (float) stepsIndex (*bars.begin()) }, { params::mglChain, bars.size() > 1 ? 1.0f : 0.0f },
                             { params::mglSlot, 0.0f }, { params::mglMix, 100.0f }, { params::mglAmount, 100.0f },
                             { params::mglTexMode, 0.0f }, { params::mglSpaceMode, 0.0f } };
                for (auto& e : extra) p.params.push_back ({ e.first, e.second });
                v.push_back (p);
            };
            add ("Start", "Clean",            { "........" });
            add ("Start", "Off-beat stutter", { ".R.R.R.R" });
            add ("Chop",  "Half-bar roll",    { "....8---" });
            add ("Chop",  "Machine gun",      { "........RRRRRRRR" });
            add ("Chop",  "Amen re-chop",     { "u.u..uu." });
            add ("Tape",  "Brake",            { "......s-" });
            add ("Tape",  "Half-time drop",   { "....h---" });
            add ("Tape",  "Reverse swell",    { "......v-" });
            add ("Tape",  "Double-time",      { "....d---" });
            add ("Tape",  "Scratch break",    { "......x-" });
            add ("Texture", "Grain shimmer",  { ".......r" }, { { params::mglTexMode, 3.0f }, { params::mglTexAmount, 60.0f }, { params::mglTexSize, 80.0f },
                                                               { params::mglTexDensity, 24.0f }, { params::mglTexScatter, 50.0f } });
            add ("Texture", "Backwards dust", { "....v---" }, { { params::mglTexMode, 4.0f }, { params::mglTexAmount, 70.0f }, { params::mglTexSize, 60.0f },
                                                               { params::mglTexDensity, 20.0f }, { params::mglTexScatter, 40.0f } });
            add ("Space", "Dub stutter",      { "......R-" }, { { params::mglSpaceMode, 1.0f }, { params::mglSpaceTime, 6.0f },
                                                               { params::mglSpaceFb, 60.0f }, { params::mglSpaceAmount, 45.0f } });
            add ("Chain", "Four-bar turnaround", { "........", "........", "........", "....R-s-" });
            add ("Chain", "Build-up",         { ".......r", "...r...r", ".r.r.r.r", "rrrrRRR8" });
            add ("Chain", "Rewind",           { "........", "........", "......v-", "v-v-x-s-" },
                                              { { params::mglSpaceMode, 2.0f }, { params::mglSpaceTime, 4.0f }, { params::mglSpaceFb, 40.0f }, { params::mglSpaceAmount, 30.0f } });
            return v;
        }();
        return list;
    }

    MangleOps::Preset MangleOps::capturePreset (const juce::String& name) const
    {
        Preset p;
        p.name = name;
        p.group = "User";
        p.slots = processor.getBridge().mangle.getAll();
        for (const char* id : { params::mglSteps, params::mglChain, params::mglMix, params::mglAmount,
                                params::mglTexMode, params::mglTexAmount, params::mglTexSize, params::mglTexDensity, params::mglTexScatter,
                                params::mglSpaceMode, params::mglSpaceTime, params::mglSpaceFb, params::mglSpaceAmount })
            p.params.push_back ({ id, processor.getParameters().getRawParameterValue (id)->load() });
        return p;
    }

    void MangleOps::applyPreset (const Preset& p)
    {
        history.clear();
        for (int k = 0; k < mangle::kSlots; ++k) processor.getBridge().mangle.set (k, p.slots[(size_t) k]);
        for (const auto& kv : p.params) setPlain (kv.first, kv.second);
        lastReport = p.name.toUpperCase();
        changed();
    }

    juce::ValueTree MangleOps::presetToTree (const Preset& p)
    {
        juce::ValueTree t ("MANGLE_PRESET");
        t.setProperty ("name", p.name, nullptr);
        t.setProperty ("version", 1, nullptr);
        for (int k = 0; k < mangle::kSlots; ++k)
        {
            auto c = p.slots[(size_t) k].toValueTree();
            c.setProperty ("slot", k, nullptr);
            t.appendChild (c, nullptr);
        }
        juce::ValueTree ps ("PARAMS");
        for (const auto& kv : p.params) ps.setProperty (kv.first, kv.second, nullptr);
        t.appendChild (ps, nullptr);
        return t;
    }

    MangleOps::Preset MangleOps::presetFromTree (const juce::ValueTree& t)
    {
        Preset p;
        p.name = t.getProperty ("name").toString();
        p.group = "User";
        for (const auto& c : t)
            if (c.hasType ("PATTERN"))
                p.slots[(size_t) juce::jlimit (0, mangle::kSlots - 1, (int) c.getProperty ("slot", 0))] = Pattern::fromValueTree (c);
        const auto ps = t.getChildWithName ("PARAMS");
        for (int i = 0; i < ps.getNumProperties(); ++i)
        {
            const auto id = ps.getPropertyName (i).toString();
            if (id.startsWith ("mgl_") && id != params::mglHold && id != params::mglFillNext && id != params::mglOn)
                p.params.push_back ({ id, (float) ps.getProperty (ps.getPropertyName (i)) });
        }
        return p;
    }

    juce::File MangleOps::userPresetFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Twisted Breaks").getChildFile ("Mangle");
    }
}
