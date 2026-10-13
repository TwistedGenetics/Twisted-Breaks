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
        };

        void prepare (double hostSampleRate) noexcept;

        /** startPpq: song position at the trigger (used to phase-lock synced loops). */
        void start (const SampleBuffer::Ptr& buffer, const Settings&, float levelFromVelocity,
                    double startPpq = 0.0, bool hostPlaying = false) noexcept;
        void release (float fadeMs = 5.0f) noexcept;
        void kill() noexcept;

        /** Live updates once per block (speed, gain, pan, region edges). */
        void update (const Settings&) noexcept;

        /** Re-locks a synced loop to the host position at the start of this block. */
        void syncTo (double ppq) noexcept;

        bool   isActive()    const noexcept { return active; }
        bool   isReleasing() const noexcept { return releasing; }
        bool   isLooping()   const noexcept { return looping; }
        double getPosition() const noexcept { return position; }
        const SampleBuffer* getBuffer() const noexcept { return buffer.get(); }

        /** Adds into `out`. Returns the voice's peak in this span (for lamps). */
        float renderAdding (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept;

        static float hermite (const float* data, int length, double pos) noexcept;

        /**
            How many beats a loop region spans, for tempo sync. Picks the
            power-of-two beat count that puts the region's own tempo between
            90 and 180 BPM (breaks, jungle and DnB), so the answer depends only
            on the audio, not on the current Live tempo.
        */
        static double loopBeatsFor (double regionSeconds) noexcept;

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
        int    regionStart = 0, regionEnd = 0;
        float  velocityGain = 1.0f;

        juce::SmoothedValue<double> increment;
        juce::SmoothedValue<float>  gainL, gainR;

        float edgeFadeSamples   = 64.0f;   // output samples
        float fadeIn            = 1.0f;    // 0..1
        float fadeInStep        = 1.0f;
        float releaseGain       = 1.0f;
        float releaseStep       = 0.005f;
    };
}
