#include "Core/MangleOps.h"
#include "PluginProcessor.h"
#include "Engine/Mangler.h"

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
        Job (juce::WeakReference<MangleOps> o, Pattern b, int s, int st, bool m, juce::AudioBuffer<float> a, int len,
             double rate, double tempo, double bar, juce::int64 seed)
            : juce::Thread ("MANGLE randomize"), owner (o), base (b), steps (s), style (st), mutateOnly (m),
              audio (std::move (a)), length (len), sr (rate), bpm (tempo), ppqPerBar (bar), rng (seed) {}

        void run() override
        {
            int kept = 0;
            const auto result = MangleOps::choose (base, steps, style, mutateOnly, audio, length, sr, bpm, ppqPerBar, rng, kept);
            auto o = owner;
            const bool listened = length > 0;
            juce::MessageManager::callAsync ([o, result, kept, listened]
            {
                if (auto* ops = o.get())
                {
                    ops->busy = false;
                    ops->lastReport = listened ? juce::String (kept) + " of 8 passed" : juce::String ("Live stopped: not checked");
                    ops->set (result, true);
                }
            });
        }

        juce::WeakReference<MangleOps> owner;
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

    Pattern MangleOps::get() const { return processor.getBridge().mangle.get(); }

    void MangleOps::set (const Pattern& p, bool recordHistory)
    {
        const auto before = get();
        if (recordHistory && before != p)
        {
            history.push_back (before);
            while (history.size() > 3) history.pop_front();
        }
        processor.getBridge().mangle.set (p);
        changed();
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
        const auto p = history.back();
        history.pop_back();
        processor.getBridge().mangle.set (p);
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
        set (p, true);
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
        worker = std::make_unique<Job> (this, get(), steps(), style(), pendingMutate, std::move (audio), got ? len : 0,
                                        sr, bpm, ppqPerBar, rng.nextInt64());
        worker->startThread (juce::Thread::Priority::low);
    }

    //==============================================================================
    juce::ValueTree MangleOps::toValueTree() const
    {
        juce::ValueTree t (mangleId);
        t.appendChild (get().toValueTree(), nullptr);
        return t;
    }

    void MangleOps::fromValueTree (const juce::ValueTree& t)
    {
        history.clear();
        processor.getBridge().mangle.set (t.isValid() ? Pattern::fromValueTree (t.getChildWithName ("PATTERN")) : Pattern{});
        if (onChange) onChange();
    }
}
