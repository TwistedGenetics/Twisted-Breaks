#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <deque>
#include "Core/MangleData.h"

class TwistedBreaksProcessor;

namespace tb
{
    /*
        MangleOps (A15)
        ---------------
        The MANGLE bar from the message thread: painting steps, RANDOMIZE,
        MUTATE, the revert arrow (the last three bars randomize / mutate
        replaced), and saving it with the set and in kit presets.

        RANDOMIZE / MUTATE listen first: they ask the engine for the last two
        bars it heard, then (on a worker thread) render 8 candidate bars over
        them, drop any that click, vanish or run away, keep the cleanest and
        match its level to the dry sound. With Live stopped (nothing to listen
        to) they write a bar without the checks.
    */
    class MangleOps final : private juce::Timer
    {
    public:
        using Pattern = mangle::Pattern;
        enum Style { gentle = 0, medium = 1, wild = 2 };

        explicit MangleOps (TwistedBreaksProcessor&);
        ~MangleOps() override;

        Pattern get() const;
        void    set (const Pattern&, bool recordHistory = false);

        // ---- Hand edits -------------------------------------------------------------
        void paint (int a, int b, int move, bool join);
        void clearStep (int step);
        void split (int step);
        void setArg (int step, int arg);
        void nudgeArg (int step, int delta);

        // ---- Randomize --------------------------------------------------------------
        void randomize() { start (false); }
        void mutate()    { start (true); }
        bool revert();
        int  historySize() const noexcept { return (int) history.size(); }
        bool isBusy() const noexcept { return busy; }

        int steps() const;
        int style() const;

        struct Check
        {
            int   clicks = 0;
            bool  vanish = false, runaway = false, silentInput = false;
            float trim = 1.0f;
            bool  ok() const noexcept { return clicks <= 2 && ! vanish && ! runaway; }
            float score() const noexcept { return (float) clicks + std::abs (std::log (juce::jmax (0.01f, trim))); }
        };

        /** A new bar (or a few changes to `base` when `mutateOnly`), unchecked. */
        static Pattern generate (const Pattern& base, int steps, int style, bool mutateOnly, juce::Random&);
        /** Renders `p` over `in` (whole bars from a bar line) and judges it against the dry sound. */
        static Check evaluate (const Pattern& p, int steps, const juce::AudioBuffer<float>& in, int length,
                               double sampleRate, double bpm, double ppqPerBar);
        /** Tries `tries` candidates and keeps the best that passes (or `base` when none does);
            `kept` gets how many passed. With no input (`length` 0) the first candidate is kept. */
        static Pattern choose (const Pattern& base, int steps, int style, bool mutateOnly,
                               const juce::AudioBuffer<float>& in, int length, double sampleRate, double bpm,
                               double ppqPerBar, juce::Random&, int& kept, int tries = 8);

        /** Synchronous RANDOMIZE / MUTATE over the given audio (tests; the async path ends here too). */
        void applyChosen (bool mutateOnly, const juce::AudioBuffer<float>& in, int length, double sampleRate, double bpm, double ppqPerBar);

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static inline const juce::Identifier mangleId { "MANGLE" };

        std::function<void()> onChange;     // the editor repaints
        juce::String getLastReport() const { return lastReport; }

    private:
        void start (bool mutateOnly);
        void timerCallback() override;
        void changed();

        TwistedBreaksProcessor& processor;
        std::deque<Pattern> history;      // newest at the back, at most 3
        juce::Random rng;
        bool busy = false, pendingMutate = false;
        uint32_t captureSeen = 0;
        int waitedMs = 0;
        juce::String lastReport;

        struct Job;
        std::unique_ptr<juce::Thread> worker;

        JUCE_DECLARE_WEAK_REFERENCEABLE (MangleOps)
    };
}
