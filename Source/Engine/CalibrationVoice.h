#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>

namespace tb
{
    /*
        CalibrationVoice  (PHASE 1 ONLY — replaced by PadVoice in Phase 2)
        ----------------
        A tiny synthesised drum voice so every pad makes sound before sample
        loading exists. This lets Phase 1 prove the whole signal path end to
        end in Ableton: MIDI note -> pad -> voice -> master -> output.

        It deliberately has the same lifecycle as the real PadVoice will:
            start (velocity)  -> 2 ms fade-in, then plays
            release()         -> 5 ms fade-out (used for retrigger/choke/panic)
            isActive()        -> false once silent, slot can be reused
        so the AudioEngine's voice allocation, retrigger crossfade and panic
        logic carry straight over to sample playback.

        Four timbres, one per pad row, each pitched per column:
            row 1  sub kick      row 2  snare (tone + noise)
            row 3  hat (noise)   row 4  dosimeter "tick" (pitched blip)
    */
    class CalibrationVoice
    {
    public:
        enum class Timbre { kick, snare, hat, tick };

        void prepare (double sampleRate) noexcept;
        void configureForPad (int padIndex) noexcept;

        void start (float velocity) noexcept;
        void release() noexcept;          // fast anti-click fade-out
        void kill() noexcept;             // immediate (only when already silent)

        bool isActive() const noexcept   { return active; }
        bool isReleasing() const noexcept { return releasing; }

        /** Adds this voice into the stereo buffer. Returns the voice's peak. */
        float renderAdding (juce::AudioBuffer<float>& out, int start, int num) noexcept;

    private:
        float nextNoise() noexcept;

        double sampleRate = 44100.0;
        Timbre timbre     = Timbre::kick;

        // Per-pad sound design
        float baseFreq    = 60.0f;   // Hz
        float sweepFreq   = 0.0f;    // Hz added at onset, decays away
        float sweepDecay  = 0.0f;    // per-sample multiplier
        float ampDecay    = 0.999f;  // per-sample multiplier
        float noiseAmt    = 0.0f;
        float noiseHpCoef = 0.0f;
        float pan         = 0.0f;    // -1..1

        // Running state
        bool   active     = false;
        bool   releasing  = false;
        float  velocity   = 1.0f;
        double phase      = 0.0;
        float  sweep      = 0.0f;
        float  amp        = 0.0f;
        float  fade       = 0.0f;    // anti-click envelope 0..1
        float  fadeInStep = 0.0f;
        float  fadeOutStep = 0.0f;
        float  noiseLp    = 0.0f;
        uint32_t rng      = 0x9E3779B9u;
    };
}
