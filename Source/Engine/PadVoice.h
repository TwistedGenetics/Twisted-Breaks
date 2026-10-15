#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Engine/SampleBuffer.h"

namespace tb
{
    /*
        PadVoice
        --------
        Plays one region of a SampleBuffer, forwards or reversed, at any
        speed, with 4-point Hermite interpolation. Two playback styles:

          ONE-SHOT  plays the region once (note-off ignored)
          LOOP      repeats the region until released; the seam is
                    crossfaded (up to 256 samples, using audio just outside
                    the region) so it loops without a click. A region that
                    starts at the very top of the file loops sample-exact.

        Host-synced loops are phase-locked to Ableton's playhead: the voice
        is told the song position each block (syncTo) and the loop's phase
        is derived from it, so a synced break stays on the grid however long
        it runs, and snaps back after a loop-brace jump or relocate.

        Click-free by construction:
          * fade-in (1.3 ms) whenever playback starts mid-file or reversed
          * fade-out (1.3 ms) as a one-shot reaches the region edge
          * release() fades over 5 ms by default (3 ms for chokes)
          * gain, pan and speed glide over ~20 ms when they change

        Holds a SampleBuffer::Ptr, so the buffer stays alive while it plays;
        dropping that reference on the audio thread is safe (see SampleBuffer).

        CHARACTER (Phase 12): vintage sampler modes. The sample is read as if
        the machine had stored it at its own rate and bit depth, so pitching
        down stretches its steps (the classic gritty SP / MPC drums) and
        pitching up aliases, as on the hardware:

            CLEAN    float, 4-point Hermite (default)
            SP-1200  26.04 kHz, 12 bit, no interpolation (drop-sample)
            MPC60    40 kHz, 12 bit, linear, gentle 2-pole low-pass ~15 kHz
            S950     31.25 kHz, 12 bit, linear, darker 2-pole low-pass ~9 kHz
            LO-FI    11.025 kHz, 8 bit, no interpolation
    */
    class PadVoice
    {
    public:
        struct Settings
        {
            int    regionStart = 0;   // source samples, inclusive
            int    regionEnd   = 0;   // source samples, exclusive
            bool   reverse     = false;
            bool   loop        = false;
            double syncPeriodBeats = 0.0;  // > 0: loop phase-locked to the host, one cycle per this many beats
            double increment   = 1.0; // source samples per output sample
            float  gainLeft    = 1.0f;
            float  gainRight   = 1.0f;
            int    character   = 0;   // params::Character (0 = CLEAN)
        };

        struct Character
        {
            const char* name;
            double rate;      // the machine's sample rate (0 = the file's own)
            int    bits;      // 0 = float
            int    interp;    // 0 none (drop-sample), 1 linear, 2 Hermite
            float  lowpassHz; // output filter, 2-pole (0 = none)
        };
        static constexpr int kNumCharacters = 5;
        static const Character& character (int i) noexcept;

        void prepare (double hostSampleRate) noexcept;

        /**
            startPpq: song position at the trigger (used to phase-lock synced loops).
            startAt >= 0: begin at this source position instead (SWAP hand-offs).
            A synced loop started that way keeps the small offset between
            startAt and its grid position for as long as it plays, so the
            attack it starts on isn't cut and it still never drifts.
        */
        void start (const SampleBuffer::Ptr& buffer, const Settings&, float levelFromVelocity,
                    double startPpq = 0.0, bool hostPlaying = false, double startAt = -1.0) noexcept;
        void release (float fadeMs = 5.0f) noexcept;
        void kill() noexcept;

        /** Live updates once per block (speed, gain, pan, region edges). */
        void update (const Settings&) noexcept;
        /** Gain / pan only (slice-sequencer voices keep their own region and speed). */
        void updateGains (const Settings& s) noexcept { if (active) { setCharacter (s.character); gainL.setTargetValue (s.gainLeft); gainR.setTargetValue (s.gainRight); } }

        /** Re-locks a synced loop to the host position at the start of this block. */
        void syncTo (double ppq) noexcept;

        /**
            A7: Push 3 pad pressure, 0..1, once per block. What it does
            (params::PressTarget):
              FILTER   2-pole low-pass closing from 18 kHz to 300 Hz
              DRIVE    tanh drive up to x16 into a level-matched saturator
              STUTTER  repeats a slice from where the press bit: 1/16, then
                       1/32 past half pressure and 1/64 near full
              PITCH    bends down up to an octave
            A synced loop that is stuttering or bent is let off the grid, and
            snaps back to it on the next block after you let go.
        */
        void setPressure (float amount, int target, double sixteenthSamples) noexcept;

        bool   isActive()    const noexcept { return active; }
        bool   isReleasing() const noexcept { return releasing; }
        bool   isLooping()   const noexcept { return looping; }
        double getPosition() const noexcept { return position; }
        double getIncrement() const noexcept { return increment.getCurrentValue(); }
        int    getRegionStart() const noexcept { return regionStart; }
        int    getRegionEnd()   const noexcept { return regionEnd; }
        bool   isReverse()      const noexcept { return reverse; }
        const SampleBuffer* getBuffer() const noexcept { return buffer.get(); }

        /** Adds into `out`. Returns the voice's peak in this span (for lamps). */
        float renderAdding (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept;

        static float hermite (const float* data, int length, double pos) noexcept;
        /** One channel at source position `pos` through character `c` (no output filter). */
        static float readCharacter (const Character& c, const float* data, int length, double pos, double sourceRate) noexcept;

        /**
            How many beats a loop region spans, for tempo sync. Picks the
            power-of-two beat count that puts the region's own tempo between
            90 and 180 BPM (breaks, jungle and DnB), so the answer depends only
            on the audio, not on the current Live tempo.
        */
        static double loopBeatsFor (double regionSeconds) noexcept;

        /** Where a synced loop with these settings sits at song position `ppq` (no offset). */
        static double gridPosition (const Settings&, double ppq) noexcept;

    private:
        double phaseLockedPosition (double ppq) const noexcept;
        int    crossfadeLength() const noexcept;

        SampleBuffer::Ptr buffer;
        double hostRate = 44100.0;

        bool   active    = false;
        bool   releasing = false;
        bool   reverse   = false;
        bool   looping   = false;
        double syncPeriod = 0.0;
        double position  = 0.0;
        double phaseOffset = 0.0;   // source samples ahead of the grid (SWAP hand-offs)
        int    regionStart = 0, regionEnd = 0;
        float  velocityGain = 1.0f;

        juce::SmoothedValue<double> increment;
        juce::SmoothedValue<float>  gainL, gainR;

        float edgeFadeSamples   = 64.0f;   // output samples
        float fadeIn            = 1.0f;    // 0..1
        float fadeInStep        = 1.0f;
        float releaseGain       = 1.0f;
        float releaseStep       = 0.005f;

        // A7 pressure.
        float  pressAmount = 0.0f;
        int    pressTarget = 4;          // params::pressOff (Parameters.h isn't included here)
        float  pressCoef   = 0.0f;
        float  pf1L = 0, pf2L = 0, pf1R = 0, pf2R = 0;
        float  driveGain = 1.0f, driveNorm = 1.0f, driveMix = 0.0f;
        double pitchMul = 1.0;
        bool   stuttering = false, stutterWrapped = false;
        bool   offGrid = false;          // a synced loop was stuttered / bent: re-lock with a fade
        static constexpr int kSnapFade = 32;
        int    snapCountdown = 0;
        double snapOffset = 0.0;
        double stutterAnchor = 0.0;
        int    stutterLen = 0, stutterCount = 0;

        int   characterIndex    = 0;
        float lpCoef            = 0.0f;                 // output filter (0 = off)
        float lp1L = 0, lp2L = 0, lp1R = 0, lp2R = 0;
        void  setCharacter (int c) noexcept;
        float read (const float* data, int length, double pos) const noexcept;
    };
}
