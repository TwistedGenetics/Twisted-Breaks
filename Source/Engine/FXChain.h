#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include "Core/FxDefs.h"
#include "Engine/Akaizer.h"

namespace tb
{
    /** Musical time for one span of audio (from Ableton, or free-running when stopped). */
    struct FxClock
    {
        double ppq          = 0.0;     // song position at the first sample
        double ppqPerSample = 0.0;     // 0 when Live is stopped
        double bpm          = 120.0;
        double ppqPerBar    = 4.0;
        bool   playing      = false;
    };

    /*
        FXChain
        -------
        The 16 performance effects of ONE group bus, processed in series in
        fixed order (spec: Pad -> Group Bus -> FX 1 -> FX 2 ... -> Group Output):

            MUTE  LPF  HPF  BPF  CRUSH  RATE  DRIVE  TAPE
            AKAIZER  REPEAT  GATE  PITCH  DELAY  REVERB  COMB  REVERSE

        Engaging / disengaging
          * insert effects crossfade dry <-> wet over 10 ms (no clicks);
          * REPEAT and REVERSE wait for the next musical boundary of
            their size, then crossfade in over 3 ms, so they land on the beat;
          * DELAY and REVERB are "throws": engaging feeds them, disengaging
            stops feeding them, and the tail rings out on its own.
          * an effect that's off (and has no tail) costs nothing.

        Capture effects keep a short history (3 s) of their own input, even
        while off, so BEAT REPEAT can repeat what just played and REVERSE can
        play the last chunk backwards.

        Everything is allocated in prepare(); process() never allocates,
        locks or touches the message thread.
    */
    class FXChain
    {
    public:
        void prepare (double sampleRate, int maxSpan);
        void reset() noexcept;                      // panic: silence everything, drop tails

        /** Once per block: which effects should be on, and their raw settings. */
        void setTargets (uint32_t onMask, const std::array<std::array<float, 3>, 16>& params) noexcept;

        /** Process a span in place. */
        void process (float* left, float* right, int numSamples, const FxClock&) noexcept;

        /** Nothing playing into the group: keep histories / clock moving (silence). */
        void advanceSilent (int numSamples, const FxClock&) noexcept;

        /** FX BYPASS: the audio skips the chain, histories still record it. */
        void recordOnly (const float* left, const float* right, int numSamples, const FxClock&) noexcept;

        /** True when no effect is on, waiting, fading or ringing: process() can be skipped. */
        bool isIdle() const noexcept;

        /** Effects audibly on now (bit per fx::Id). */
        uint32_t engagedMask() const noexcept;
        /** Effects waiting for their boundary. */
        uint32_t armedMask() const noexcept;

    private:
        // ---- Shared helpers -----------------------------------------------------
        struct History
        {
            juce::AudioBuffer<float> buf;
            int64_t written = 0;              // absolute sample index of the next write
            int     size = 0;
            void prepare (int frames)         { size = frames; buf.setSize (2, frames, false, true, false); buf.clear(); written = 0; }
            void write (float l, float r) noexcept
            {
                const int i = (int) (written % size);
                buf.setSample (0, i, l); buf.setSample (1, i, r);
                ++written;
            }
            void writeSilence (int n) noexcept;
            float read (int ch, int64_t abs) const noexcept
            {
                if (abs < 0 || abs >= written || written - abs > size) return 0.0f;
                return buf.getSample (ch, (int) (abs % size));
            }
            float readInterp (int ch, double abs) const noexcept
            {
                const auto i = (int64_t) std::floor (abs);
                const float f = (float) (abs - (double) i);
                return read (ch, i) + (read (ch, i + 1) - read (ch, i)) * f;
            }
        };

        /** Loops a stretch of history (STUTTER / REPEAT) or plays chunks backwards (REVERSE). */
        struct Looper
        {
            History hist;
            enum class State { off, armed, on } state = State::off;
            double  sizeBeats = 0.25;
            int64_t loopStart = 0, playStart = 0;
            int     loopLen = 0;
            float   wet = 0.0f, wetTarget = 0.0f;
            bool    want = false;
        };

        float  beatsToSamples (double beats, const FxClock&) const noexcept;
        double nextBoundary (double ppq, double beats) const noexcept;

        void processLooper (int id, Looper&, float* l, float* r, int n, double ppq0, double dppq, const FxClock&) noexcept;
        void processReverse (float* l, float* r, int n, double ppq0, double dppq, const FxClock&) noexcept;

        double sr = 48000.0;
        int    maxSpan = 512;
        double freePpq = 0.0;          // clock while Live is stopped

        uint32_t targetMask = 0;
        std::array<std::array<float, 3>, 16> prm {};

        // Insert-effect crossfades (10 ms), indexed by fx::Id.
        std::array<juce::SmoothedValue<float>, 16> wet;

        // Filters
        std::array<juce::dsp::StateVariableTPTFilter<float>, 3> filters;
        std::array<juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>, 3> cutoff;

        // Crush / rate
        std::array<float, 2> holdValue {};
        double ratePhase = 1.0;

        // Tape tone (one-pole low-pass state)
        std::array<float, 2> tapeLp {};

        // Gate
        float gateGain = 1.0f;

        // Capture effects
        Looper repeatL, reverseL;
        AkaiShifter akai;                          // A12: AKAIZER (STUTTER's place)
        History pitchHist;
        bool   pitchActive = false;
        double pitchRead = 0.0, pitchRate = 1.0, pitchRateStep = 0.0;

        // Delay
        juce::AudioBuffer<float> delayBuf;
        int    delayWrite = 0;
        juce::SmoothedValue<float> delayTime;     // samples
        std::array<float, 2> delayLp {};
        bool   delayRinging = false;              // echoes still in the line
        int    delaySilent  = 0;                  // samples since anything audible went through

        // Reverb
        juce::Reverb freeverb;
        juce::AudioBuffer<float> scratch;
        float  reverbTail = 0.0f;

        // Comb
        juce::AudioBuffer<float> combBuf;
        int    combWrite = 0;
        std::array<float, 2> combLp {};
        juce::SmoothedValue<float> combDelay;     // samples
    };
}
