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

        A step holds more than version 1 uses, so later versions can switch
        features on without changing the saved format:

            bit  0      on
            bits 1-7    velocity 1..127
            bits 8-15   micro-timing, signed, in 1/128 of a step (-64..63)
            bits 16-22  probability 0..100       (future: played as 100)
            bits 23-24  ratchet 1..4             (future: played as 1)
            bits 25-30  pitch -32..31 semitones  (future: played as 0)

        Live recording finer than the step grid (1/32 on a 1/16 grid) lands on
        the nearest step with micro-timing, so it plays back where it was hit.
    */
    struct SeqStep
    {
        bool on = false;
        int  velocity = 100;
        int  micro = 0;          // 1/128 step, -64..63
        int  probability = 100;
        int  ratchet = 1;
        int  pitch = 0;

        uint32_t pack() const noexcept
        {
            return (on ? 1u : 0u)
                 | ((uint32_t) juce::jlimit (1, 127, velocity) << 1)
                 | ((uint32_t) (uint8_t) (int8_t) juce::jlimit (-64, 63, micro) << 8)
                 | ((uint32_t) juce::jlimit (0, 100, probability) << 16)
                 | ((uint32_t) (juce::jlimit (1, 4, ratchet) - 1) << 23)
                 | ((uint32_t) (juce::jlimit (-32, 31, pitch) + 32) << 25);
        }

        static SeqStep unpack (uint32_t v) noexcept
        {
            SeqStep s;
            s.on          = (v & 1u) != 0;
            s.velocity    = juce::jmax (1, (int) ((v >> 1) & 0x7fu));
            s.micro       = (int) (int8_t) (uint8_t) ((v >> 8) & 0xffu);
            s.probability = (int) ((v >> 16) & 0x7fu);
            s.ratchet     = (int) ((v >> 23) & 0x3u) + 1;
            s.pitch       = (int) ((v >> 25) & 0x3fu) - 32;
            return s;
        }

        bool operator== (const SeqStep& o) const noexcept { return pack() == o.pack(); }
    };

    class SequencerData
    {
    public:
        static constexpr int kPads  = 16;
        static constexpr int kSteps = 32;

        SequencerData() { clearAll(); }

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
                        steps[(size_t) p][(size_t) s].store ((uint32_t) v[s].getHexValue64());
            }
            version.fetch_add (1);
        }

    private:
        static bool valid (int pad, int step) noexcept { return pad >= 0 && pad < kPads && step >= 0 && step < kSteps; }

        std::array<std::array<std::atomic<uint32_t>, kSteps>, kPads> steps;
        std::array<std::atomic<bool>, kPads> armed {};
        std::atomic<uint32_t> version { 0 };
    };
}
