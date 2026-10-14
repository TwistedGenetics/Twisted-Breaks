#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <cmath>
#include "Engine/SliceTable.h"

namespace tb
{
    /*
        Slice sequencer data (spec addition A5)
        ---------------------------------------
        Every pad has 8 slots, each a pattern of up to 32 steps that plays the
        pad's slices in any order. Shared lock-free like the main sequencer:
        one 64-bit atomic per step, one 32-bit atomic per slot's settings.

        Step (64 bits):
            kind      PLAY a slice / REST (cut) / TIE (let it ring on)
            slice     1..16
            rev       play that slice backwards
            pitch     -12..+12 semitones
            velocity  1..127
            length    gate: 1/4, 1/2, 3/4 of the step, or FULL (until the next hit)
            ratchet   1..4 hits inside the step (rolls)
            chance    0..100 %
            micro     -32..31 in 1/64 of a step (half a step either way)

        Slot settings (32 bits): length 1..32, rate (1/8, 1/16, 1/32, 1/16T),
        swing 0..100 %, and whether the slot has been written yet. An unwritten
        slot plays the DEFAULT pattern: each slice on its nearest step, with
        micro-timing for its exact start, so an untouched pattern replays the
        break exactly as recorded (at any tempo: it's re-sequenced, not
        stretched).
    */
    struct SliceStep
    {
        enum Kind { play = 0, rest = 1, tie = 2 };
        int  kind = tie;
        int  slice = 0;          // 0-based
        bool rev = false;
        int  pitch = 0;
        int  velocity = 127;
        int  length = 3;         // index: 1/4, 1/2, 3/4, FULL
        int  ratchet = 1;
        int  chance = 100;
        int  micro = 0;          // 1/64 step

        uint64_t pack() const noexcept
        {
            uint64_t v = 0;
            v |= (uint64_t) juce::jlimit (0, 2, kind);
            v |= (uint64_t) juce::jlimit (0, 15, slice) << 2;
            v |= (uint64_t) (rev ? 1 : 0) << 6;
            v |= (uint64_t) (juce::jlimit (-12, 12, pitch) + 16) << 7;          // 5 bits
            v |= (uint64_t) juce::jlimit (1, 127, velocity) << 12;              // 7 bits
            v |= (uint64_t) juce::jlimit (0, 3, length) << 19;                  // 2 bits
            v |= (uint64_t) (juce::jlimit (1, 4, ratchet) - 1) << 21;           // 2 bits
            v |= (uint64_t) juce::jlimit (0, 100, chance) << 23;                // 7 bits
            v |= (uint64_t) (juce::jlimit (-32, 31, micro) + 32) << 30;         // 6 bits
            return v;
        }

        static SliceStep unpack (uint64_t v) noexcept
        {
            SliceStep s;
            s.kind     = (int) (v & 3u);
            s.slice    = (int) ((v >> 2) & 15u);
            s.rev      = ((v >> 6) & 1u) != 0;
            s.pitch    = (int) ((v >> 7) & 31u) - 16;
            s.velocity = juce::jmax (1, (int) ((v >> 12) & 127u));
            s.length   = (int) ((v >> 19) & 3u);
            s.ratchet  = (int) ((v >> 21) & 3u) + 1;
            s.chance   = (int) ((v >> 23) & 127u);
            s.micro    = (int) ((v >> 30) & 63u) - 32;
            return s;
        }

        static SliceStep playing (int slice, int micro = 0)
        {
            SliceStep s; s.kind = play; s.slice = slice; s.micro = micro; return s;
        }

        bool operator== (const SliceStep& o) const noexcept { return pack() == o.pack(); }
    };

    struct SliceSlot
    {
        int  length = 16;
        int  rate = 1;           // 0 = 1/8, 1 = 1/16, 2 = 1/32, 3 = 1/16T
        int  swing = 0;          // 0..100 %
        bool written = false;

        uint32_t pack() const noexcept
        {
            return (uint32_t) juce::jlimit (1, 32, length)
                 | ((uint32_t) juce::jlimit (0, 3, rate) << 6)
                 | ((uint32_t) juce::jlimit (0, 100, swing) << 8)
                 | ((written ? 1u : 0u) << 15);
        }
        static SliceSlot unpack (uint32_t v) noexcept
        {
            SliceSlot s;
            s.length  = juce::jlimit (1, 32, (int) (v & 63u));
            s.rate    = (int) ((v >> 6) & 3u);
            s.swing   = juce::jlimit (0, 100, (int) ((v >> 8) & 127u));
            s.written = ((v >> 15) & 1u) != 0;
            return s;
        }
    };

    inline constexpr double kSliceRateBeats[] = { 0.5, 0.25, 0.125, 1.0 / 6.0 };
    inline const char* const kSliceRateNames[] = { "1/8", "1/16", "1/32", "1/16T" };
    inline const char* const kSliceLengthNames[] = { "1/4", "1/2", "3/4", "FULL" };
    inline constexpr float  kSliceLengthFrac[] = { 0.25f, 0.5f, 0.75f, 1.0f };

    class SliceSeqData
    {
    public:
        static constexpr int kPads = 32, kSlots = 8, kSteps = 32;    // kPads = kNumPads (two scenes)

        SliceSeqData() { clearAll(); }

        SliceStep getStep (int pad, int slot, int step) const noexcept
        {
            if (! valid (pad, slot, step)) return {};
            return SliceStep::unpack (steps[(size_t) pad][(size_t) slot][(size_t) step].load (std::memory_order_relaxed));
        }
        SliceSlot getSlot (int pad, int slot) const noexcept
        {
            if (! valid (pad, slot, 0)) return {};
            return SliceSlot::unpack (slots[(size_t) pad][(size_t) slot].load (std::memory_order_relaxed));
        }

        /** Writing a step marks the slot written. Any thread, lock-free. */
        void setStep (int pad, int slot, int step, const SliceStep& s) noexcept
        {
            if (! valid (pad, slot, step)) return;
            steps[(size_t) pad][(size_t) slot][(size_t) step].store (s.pack(), std::memory_order_relaxed);
            auto sl = getSlot (pad, slot);
            if (! sl.written) { sl.written = true; slots[(size_t) pad][(size_t) slot].store (sl.pack()); }
            bump();
        }
        void setSlot (int pad, int slot, const SliceSlot& s) noexcept
        {
            if (! valid (pad, slot, 0)) return;
            slots[(size_t) pad][(size_t) slot].store (s.pack(), std::memory_order_relaxed);
            bump();
        }

        /** Back to "unwritten": plays the default (the break as recorded). */
        void resetSlot (int pad, int slot) noexcept
        {
            if (! valid (pad, slot, 0)) return;
            for (auto& st : steps[(size_t) pad][(size_t) slot]) st.store (SliceStep{}.pack());
            slots[(size_t) pad][(size_t) slot].store (SliceSlot{}.pack());
            bump();
        }
        void clearAll() noexcept
        {
            for (int p = 0; p < kPads; ++p)
                for (int s = 0; s < kSlots; ++s)
                {
                    for (auto& st : steps[(size_t) p][(size_t) s]) st.store (SliceStep{}.pack());
                    slots[(size_t) p][(size_t) s].store (SliceSlot{}.pack());
                }
            bump();
        }

        uint32_t getVersion() const noexcept { return version.load (std::memory_order_relaxed); }

        // Raw packed access (snapshot switching: copied as plain numbers).
        uint64_t getPackedStep (int p, int s, int k) const noexcept { return valid (p, s, k) ? steps[(size_t) p][(size_t) s][(size_t) k].load (std::memory_order_relaxed) : 0; }
        uint32_t getPackedSlot (int p, int s) const noexcept { return valid (p, s, 0) ? slots[(size_t) p][(size_t) s].load (std::memory_order_relaxed) : 0; }
        void setPackedStep (int p, int s, int k, uint64_t v) noexcept { if (valid (p, s, k)) steps[(size_t) p][(size_t) s][(size_t) k].store (v, std::memory_order_relaxed); }
        void setPackedSlot (int p, int s, uint32_t v) noexcept { if (valid (p, s, 0)) slots[(size_t) p][(size_t) s].store (v, std::memory_order_relaxed); }
        void bumpVersion() noexcept { bump(); }

        // ---- Default pattern ---------------------------------------------------------
        /**
            Each slice on its nearest step of `stepBeats`, with micro-timing for
            its exact start; steps without a slice start are TIEs (the previous
            slice rings on). `sampleLength` / `sourceRate` give the break's
            length in beats (via the same 90-180 BPM reading as synced loops).
        */
        static int defaultPattern (const SliceTable& slices, int sampleLength, double sourceRate, double stepBeats,
                                   double beatsInBreak, std::array<SliceStep, kSteps>& out) noexcept
        {
            for (auto& s : out) s = SliceStep{};
            const int stepsInBreak = juce::jlimit (1, kSteps, (int) std::lround (beatsInBreak / stepBeats));
            const double stepLen = (double) sampleLength / (beatsInBreak / stepBeats);
            juce::ignoreUnused (sourceRate);

            const SliceTable t = slices.isEmpty() ? SliceTable::equal (sampleLength, juce::jmin (16, stepsInBreak)) : slices;
            for (int i = 0; i < t.count; ++i)
            {
                const double x = (double) t.starts[(size_t) i] / stepLen;
                const int k = (int) std::lround (x);
                if (k < 0 || k >= stepsInBreak) continue;
                if (out[(size_t) k].kind == SliceStep::play) continue;   // two slices on one step: keep the first
                out[(size_t) k] = SliceStep::playing (i, juce::jlimit (-32, 31, (int) std::lround ((x - k) * 64.0)));
            }
            if (out[0].kind != SliceStep::play) out[0] = SliceStep::playing (0);
            return stepsInBreak;
        }

        // ---- State -----------------------------------------------------------------------
        static inline const juce::Identifier sliceSeqId { "SLICESEQ" };

        juce::ValueTree toValueTree() const
        {
            juce::ValueTree t (sliceSeqId);
            for (int p = 0; p < kPads; ++p)
                for (int s = 0; s < kSlots; ++s)
                {
                    const auto sl = getSlot (p, s);
                    if (! sl.written && sl.pack() == SliceSlot{}.pack()) continue;
                    juce::ValueTree n ("SLOT");
                    n.setProperty ("pad", p, nullptr);
                    n.setProperty ("slot", s, nullptr);
                    n.setProperty ("settings", (juce::int64) sl.pack(), nullptr);
                    juce::StringArray v;
                    for (int k = 0; k < kSteps; ++k)
                        v.add (juce::String::toHexString ((juce::int64) steps[(size_t) p][(size_t) s][(size_t) k].load()));
                    n.setProperty ("steps", v.joinIntoString (","), nullptr);
                    t.appendChild (n, nullptr);
                }
            return t;
        }

        void fromValueTree (const juce::ValueTree& t)
        {
            clearAll();
            for (const auto& n : t)
            {
                const int p = (int) n.getProperty ("pad", -1), s = (int) n.getProperty ("slot", -1);
                if (! valid (p, s, 0)) continue;
                slots[(size_t) p][(size_t) s].store ((uint32_t) (juce::int64) n.getProperty ("settings", 0));
                const auto v = juce::StringArray::fromTokens (n.getProperty ("steps", "").toString(), ",", "");
                for (int k = 0; k < juce::jmin (kSteps, v.size()); ++k)
                    steps[(size_t) p][(size_t) s][(size_t) k].store ((uint64_t) v[k].getHexValue64());
            }
            bump();
        }

    private:
        static bool valid (int p, int s, int k) noexcept
        {
            return p >= 0 && p < kPads && s >= 0 && s < kSlots && k >= 0 && k < kSteps;
        }
        void bump() noexcept { version.fetch_add (1, std::memory_order_relaxed); }

        std::array<std::array<std::array<std::atomic<uint64_t>, kSteps>, kSlots>, kPads> steps;
        std::array<std::array<std::atomic<uint32_t>, kSlots>, kPads> slots;
        std::atomic<uint32_t> version { 0 };
    };
}
