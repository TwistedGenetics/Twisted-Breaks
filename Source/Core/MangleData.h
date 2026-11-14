#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>

namespace tb
{
    /*
        MANGLE pattern (A15, 0.29)
        --------------------------
        One bar of up to 16 steps. Each step has a move, a "tied to the step
        before" flag (joined steps play one longer move) and an argument
        (REPEAT: 2 / 4 / 8 slices; SHUFFLE: which step of the last bar to play).
        A run is a step plus the steps tied after it; the run's first step
        holds its move.

        `MangleData` keeps the live pattern as atomics, so the editor writes
        while the audio thread reads (one step may be a block behind, never
        torn). `MangleData::Pattern` is a plain copy for the message thread,
        randomize and tests.
    */
    namespace mangle
    {
        enum Move : uint8_t { thru = 0, repeat, reverse, half, stop, dbl, scratch, freeze, shuffle, kNumMoves };

        inline const char* const kMoveNames[] = { "THRU", "REPEAT", "REVERSE", "HALF", "STOP", "DOUBLE", "SCRATCH", "FREEZE", "SHUFFLE" };
        inline const char* const kMoveShort[] = { "THRU", "RPT", "REV", "HALF", "STOP", "DBL", "SCR", "FRZ", "SHUF" };
        inline const char* const kMoveKeys[]  = { "thru", "repeat", "reverse", "half", "stop", "double", "scratch", "freeze", "shuffle" };
        inline constexpr int kRepeatSlices[] = { 2, 4, 8 };

        inline int moveFromKey (const juce::String& k)
        {
            for (int i = 0; i < kNumMoves; ++i) if (k == kMoveKeys[i]) return i;
            return thru;
        }

        struct Pattern
        {
            static constexpr int kSteps = 16;
            std::array<uint8_t, kSteps> move {};   // Move
            std::array<uint8_t, kSteps> tie {};    // 1: joined to the step before (step 0 never)
            std::array<uint8_t, kSteps> arg {};    // REPEAT: index into kRepeatSlices; SHUFFLE: source step
            float trim = 1.0f;                     // wet level match (randomize)

            bool operator== (const Pattern& o) const noexcept
            {
                return move == o.move && tie == o.tie && arg == o.arg && std::abs (trim - o.trim) < 1.0e-4f;
            }
            bool operator!= (const Pattern& o) const noexcept { return ! (*this == o); }

            /** First step of the run that `step` belongs to (within `steps`). */
            int runStart (int step) const noexcept
            {
                while (step > 0 && tie[(size_t) step] != 0) --step;
                return step;
            }
            /** Number of steps in the run starting at `start` (within `steps`). */
            int runLength (int start, int steps) const noexcept
            {
                int e = start + 1;
                while (e < steps && tie[(size_t) e] != 0) ++e;
                return e - start;
            }
            /** Sets a move over steps a..b (inclusive): they become one run when `join`. */
            void paint (int a, int b, int m, bool join) noexcept
            {
                if (a > b) std::swap (a, b);
                a = juce::jlimit (0, kSteps - 1, a);
                b = juce::jlimit (0, kSteps - 1, b);
                // Splitting runs that stick out of a..b keeps their outside parts as they were.
                if (a > 0 && tie[(size_t) a] != 0)       tie[(size_t) a] = 0;
                if (b + 1 < kSteps && tie[(size_t) b + 1] != 0)
                {
                    tie[(size_t) b + 1] = 0;
                    const int owner = runStart (b);
                    move[(size_t) b + 1] = move[(size_t) owner];
                    arg[(size_t) b + 1]  = arg[(size_t) owner];
                }
                for (int s = a; s <= b; ++s)
                {
                    move[(size_t) s] = (uint8_t) juce::jlimit (0, (int) kNumMoves - 1, m);
                    tie[(size_t) s]  = (uint8_t) (join && s > a ? 1 : 0);
                    if (m == shuffle && arg[(size_t) s] >= kSteps) arg[(size_t) s] = 0;
                }
            }
            /** Splits the run that holds `step` back into single steps (each keeps the move). */
            void split (int step) noexcept
            {
                const int s0 = runStart (step);
                const int n = runLength (s0, kSteps);
                for (int s = s0; s < s0 + n; ++s) { tie[(size_t) s] = 0; move[(size_t) s] = move[(size_t) s0]; arg[(size_t) s] = arg[(size_t) s0]; }
            }
            int activeSteps (int steps) const noexcept
            {
                int n = 0;
                for (int s = 0; s < steps; ++s) if (move[(size_t) runStart (s)] != thru) ++n;
                return n;
            }

            juce::ValueTree toValueTree() const
            {
                juce::ValueTree t ("PATTERN");
                juce::String mv, tv, av;
                for (int s = 0; s < kSteps; ++s)
                {
                    mv << kMoveKeys[juce::jlimit (0, (int) kNumMoves - 1, (int) move[(size_t) s])] << (s < kSteps - 1 ? "," : "");
                    tv << (int) tie[(size_t) s];
                    av << (int) arg[(size_t) s] << (s < kSteps - 1 ? "," : "");
                }
                t.setProperty ("moves", mv, nullptr);
                t.setProperty ("ties", tv, nullptr);
                t.setProperty ("args", av, nullptr);
                t.setProperty ("trim", trim, nullptr);
                return t;
            }
            static Pattern fromValueTree (const juce::ValueTree& t)
            {
                Pattern p;
                if (! t.isValid()) return p;
                const auto mv = juce::StringArray::fromTokens (t.getProperty ("moves").toString(), ",", "");
                const auto av = juce::StringArray::fromTokens (t.getProperty ("args").toString(), ",", "");
                const auto tv = t.getProperty ("ties").toString();
                for (int s = 0; s < kSteps; ++s)
                {
                    if (s < mv.size()) p.move[(size_t) s] = (uint8_t) moveFromKey (mv[s].trim());
                    if (s < av.size()) p.arg[(size_t) s]  = (uint8_t) juce::jlimit (0, 15, av[s].getIntValue());
                    if (s > 0 && s < tv.length()) p.tie[(size_t) s] = tv[s] == '1' ? 1 : 0;
                }
                p.trim = juce::jlimit (0.25f, 4.0f, (float) t.getProperty ("trim", 1.0f));
                return p;
            }
        };
    }

    class MangleData
    {
    public:
        using Pattern = mangle::Pattern;
        static constexpr int kSteps = Pattern::kSteps;

        MangleData() { set (Pattern{}); }

        // ---- Any thread -------------------------------------------------------------
        Pattern get() const noexcept
        {
            Pattern p;
            for (int s = 0; s < kSteps; ++s)
            {
                p.move[(size_t) s] = move[(size_t) s].load (std::memory_order_relaxed);
                p.tie[(size_t) s]  = tie[(size_t) s].load (std::memory_order_relaxed);
                p.arg[(size_t) s]  = arg[(size_t) s].load (std::memory_order_relaxed);
            }
            p.trim = trim.load (std::memory_order_relaxed);
            return p;
        }

        // ---- Message thread -------------------------------------------------------------
        void set (const Pattern& p) noexcept
        {
            for (int s = 0; s < kSteps; ++s)
            {
                move[(size_t) s].store (p.move[(size_t) s], std::memory_order_relaxed);
                tie[(size_t) s].store (s == 0 ? 0 : p.tie[(size_t) s], std::memory_order_relaxed);
                arg[(size_t) s].store (p.arg[(size_t) s], std::memory_order_relaxed);
            }
            trim.store (p.trim, std::memory_order_relaxed);
            version.fetch_add (1, std::memory_order_release);
        }
        uint32_t getVersion() const noexcept { return version.load (std::memory_order_acquire); }

    private:
        std::array<std::atomic<uint8_t>, kSteps> move {}, tie {}, arg {};
        std::atomic<float> trim { 1.0f };
        std::atomic<uint32_t> version { 0 };
    };
}
