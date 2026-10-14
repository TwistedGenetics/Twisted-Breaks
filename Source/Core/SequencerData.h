#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>

namespace tb
{
    /*
        SequencerData
        -------------
        The 32-step pattern: one lane per pad, 32 steps per lane. Shared by the
        UI (editing), the audio thread (playback and live recording) and the
        state code (saving), so every step is ONE packed 32-bit atomic: no
        locks, no queues, nothing for the audio thread to allocate or wait on.

        A step is ONE packed 64-bit atomic (0.9.0; 0.7/0.8 sets saved 32 bits,
        which load unchanged: the new fields read as "off"):

            bit  0      on
            bits 1-7    velocity 1..127
            bits 8-15   micro-timing, signed, in 1/128 of a step (-64..63)
            bits 16-22  probability 0..100 %
            bits 23-24  ratchet 1..4 (hits spread evenly across the step)
            bits 25-30  pitch -32..31 semitones (UI: +-24), on top of the pad's
            bit  31     reverse (flips the pad's direction for this hit)
            bits 32-36  slice: 0 = the pad's own region, 1..16 = play that slice
            bits 37-41  FX: 0 = none, 1..16 = hold that effect on the pad's
                        group for the length of the step
            bits 42-46  condition (kSeqCondNames): ALWAYS, 1:2 .. 4:4, FILL,
                        NOT FILL, FIRST, NOT FIRST
            bit  47     no hit: the step only throws its FX (a break keeps
                        running underneath instead of retriggering)

        Live recording finer than the step grid (1/32 on a 1/16 grid) lands on
        the nearest step with micro-timing, so it plays back where it was hit.
    */
    /** Step conditions. A:B = plays on pass A of every B passes of the pattern
        (counted on Live's timeline, so a 4-bar loop brace repeats them).
        FIRST = the first pass after Live starts. Append only: saved. */
    enum SeqCond { condAlways = 0, cond1of2, cond2of2, cond1of3, cond2of3, cond3of3,
                   cond1of4, cond2of4, cond3of4, cond4of4, condFill, condNotFill,
                   condFirst, condNotFirst, kNumSeqConds };
    inline const char* const kSeqCondNames[] = { "ALWAYS", "1:2", "2:2", "1:3", "2:3", "3:3",
                                                 "1:4", "2:4", "3:4", "4:4", "FILL", "!FILL",
                                                 "FIRST", "!FIRST" };

    /** Does a step with condition `c` play on pattern pass `pass`? */
    inline bool seqConditionPasses (int c, int64_t pass, bool fill, bool firstPass) noexcept
    {
        auto ab = [pass] (int a, int b) { return ((pass % b) + b) % b == a - 1; };
        switch (c)
        {
            case cond1of2: return ab (1, 2);  case cond2of2: return ab (2, 2);
            case cond1of3: return ab (1, 3);  case cond2of3: return ab (2, 3);  case cond3of3: return ab (3, 3);
            case cond1of4: return ab (1, 4);  case cond2of4: return ab (2, 4);
            case cond3of4: return ab (3, 4);  case cond4of4: return ab (4, 4);
            case condFill:     return fill;
            case condNotFill:  return ! fill;
            case condFirst:    return firstPass;
            case condNotFirst: return ! firstPass;
            default:           return true;
        }
    }

    struct SeqStep
    {
        bool on = false;
        int  velocity = 100;
        int  micro = 0;          // 1/128 step, -64..63
        int  probability = 100;
        int  ratchet = 1;
        int  pitch = 0;          // semitones
        bool rev = false;
        int  slice = 0;          // 0 = pad's own, 1..16
        int  fx = 0;             // 0 = none, 1..16 = fx::Id + 1
        int  cond = condAlways;
        bool hit = true;         // false = FX only

        /** Anything that changes HOW the pad plays (needs its own voice settings). */
        bool hasSoundTweak() const noexcept { return pitch != 0 || rev || slice > 0; }

        uint64_t pack() const noexcept
        {
            return (on ? 1u : 0u)
                 | ((uint64_t) juce::jlimit (1, 127, velocity) << 1)
                 | ((uint64_t) (uint8_t) (int8_t) juce::jlimit (-64, 63, micro) << 8)
                 | ((uint64_t) juce::jlimit (0, 100, probability) << 16)
                 | ((uint64_t) (juce::jlimit (1, 4, ratchet) - 1) << 23)
                 | ((uint64_t) (juce::jlimit (-32, 31, pitch) + 32) << 25)
                 | ((uint64_t) (rev ? 1u : 0u) << 31)
                 | ((uint64_t) juce::jlimit (0, 16, slice) << 32)
                 | ((uint64_t) juce::jlimit (0, 16, fx) << 37)
                 | ((uint64_t) juce::jlimit (0, kNumSeqConds - 1, cond) << 42)
                 | ((uint64_t) (hit ? 0u : 1u) << 47);
        }

        static SeqStep unpack (uint64_t v) noexcept
        {
            SeqStep s;
            s.on          = (v & 1u) != 0;
            s.velocity    = juce::jmax (1, (int) ((v >> 1) & 0x7fu));
            s.micro       = (int) (int8_t) (uint8_t) ((v >> 8) & 0xffu);
            s.probability = juce::jmin (100, (int) ((v >> 16) & 0x7fu));
            s.ratchet     = (int) ((v >> 23) & 0x3u) + 1;
            s.pitch       = (int) ((v >> 25) & 0x3fu) - 32;
            s.rev         = ((v >> 31) & 1u) != 0;
            s.slice       = juce::jmin (16, (int) ((v >> 32) & 0x1fu));
            s.fx          = juce::jmin (16, (int) ((v >> 37) & 0x1fu));
            s.cond        = juce::jmin (kNumSeqConds - 1, (int) ((v >> 42) & 0x1fu));
            s.hit         = ((v >> 47) & 1u) == 0;
            return s;
        }

        bool operator== (const SeqStep& o) const noexcept { return pack() == o.pack(); }
    };

    class SequencerData
    {
    public:
        static constexpr int kPads  = 32;      // two scenes of 16 (= kNumPads)
        static constexpr int kSteps = 32;

        SequencerData() { clearAll(); }

        static_assert (std::atomic<uint64_t>::is_always_lock_free, "steps must be lock-free");

        SeqStep get (int pad, int step) const noexcept
        {
            if (! valid (pad, step)) return {};
            return SeqStep::unpack (steps[(size_t) pad][(size_t) step].load (std::memory_order_relaxed));
        }

        /** Any thread, never blocks or allocates. */
        void set (int pad, int step, const SeqStep& s) noexcept
        {
            if (! valid (pad, step)) return;
            steps[(size_t) pad][(size_t) step].store (s.pack(), std::memory_order_relaxed);
            version.fetch_add (1, std::memory_order_relaxed);
        }

        void clearLane (int pad) noexcept
        {
            if (pad < 0 || pad >= kPads) return;
            for (auto& s : steps[(size_t) pad]) s.store (SeqStep{}.pack(), std::memory_order_relaxed);
            version.fetch_add (1, std::memory_order_relaxed);
        }

        void clearAll() noexcept
        {
            for (int p = 0; p < kPads; ++p)
                for (auto& s : steps[(size_t) p]) s.store (SeqStep{}.pack(), std::memory_order_relaxed);
            version.fetch_add (1, std::memory_order_relaxed);
        }

        // Raw packed access (snapshot switching: copied as plain numbers).
        uint64_t getPacked (int pad, int step) const noexcept { return valid (pad, step) ? steps[(size_t) pad][(size_t) step].load (std::memory_order_relaxed) : 0; }
        void setPacked (int pad, int step, uint64_t v) noexcept { if (valid (pad, step)) steps[(size_t) pad][(size_t) step].store (v, std::memory_order_relaxed); }
        void bumpVersion() noexcept { version.fetch_add (1, std::memory_order_relaxed); }

        bool laneHasSteps (int pad) const noexcept
        {
            for (int s = 0; s < kSteps; ++s) if (get (pad, s).on) return true;
            return false;
        }

        // ---- Record arm (one lane captures pad hits) ------------------------------
        void setArmed (int pad, bool a) noexcept { if (pad >= 0 && pad < kPads) { armed[(size_t) pad].store (a); version.fetch_add (1); } }
        bool isArmed (int pad) const noexcept   { return pad >= 0 && pad < kPads && armed[(size_t) pad].load (std::memory_order_relaxed); }

        /** Bumps on every change (UI repaint, host "state changed"). */
        uint32_t getVersion() const noexcept { return version.load (std::memory_order_relaxed); }

        // ---- State ---------------------------------------------------------------------
        static inline const juce::Identifier seqId { "SEQ" };

        juce::ValueTree toValueTree() const
        {
            juce::ValueTree t (seqId);
            for (int p = 0; p < kPads; ++p)
            {
                if (! laneHasSteps (p)) continue;
                juce::ValueTree lane ("LANE");
                lane.setProperty ("pad", p, nullptr);
                juce::StringArray v;
                for (int s = 0; s < kSteps; ++s)
                    v.add (juce::String::toHexString ((juce::int64) steps[(size_t) p][(size_t) s].load()));
                lane.setProperty ("steps", v.joinIntoString (","), nullptr);
                t.appendChild (lane, nullptr);
            }
            return t;
        }

        void fromValueTree (const juce::ValueTree& t)
        {
            clearAll();
            for (const auto& lane : t)
            {
                const int p = (int) lane.getProperty ("pad", -1);
                const auto v = juce::StringArray::fromTokens (lane.getProperty ("steps", "").toString(), ",", "");
                for (int s = 0; s < juce::jmin (kSteps, v.size()); ++s)
                    if (valid (p, s))
                        steps[(size_t) p][(size_t) s].store ((uint64_t) v[s].getHexValue64());
            }
            version.fetch_add (1);
        }

    private:
        static bool valid (int pad, int step) noexcept { return pad >= 0 && pad < kPads && step >= 0 && step < kSteps; }

        std::array<std::array<std::atomic<uint64_t>, kSteps>, kPads> steps;
        std::array<std::atomic<bool>, kPads> armed {};
        std::atomic<uint32_t> version { 0 };
    };
}
