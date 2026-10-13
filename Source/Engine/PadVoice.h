#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Engine/SampleBuffer.h"

namespace tb
{
    /*
        PadVoice
        --------
        Plays one region of a SampleBuffer, forwards or reversed, at any
        pitch, with 4-point Hermite interpolation.

        Click-free by construction:
          * fade-in (1.3 ms) whenever playback starts mid-file or reversed
          * fade-out (1.3 ms) as the read head approaches the region edge
          * release() fades over 5 ms (retrigger, panic, sample swap)
          * gain, pan and pitch glide over ~20 ms when they change

        The region's far edge is live: moving END (or START when reversed)
        while a voice plays takes effect immediately.

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
            double increment   = 1.0; // source samples per output sample
            float  gainLeft    = 1.0f;
            float  gainRight   = 1.0f;
        };

        void prepare (double hostSampleRate) noexcept;

        void start (const SampleBuffer::Ptr& buffer, const Settings&, float velocity) noexcept;
        void release() noexcept;
        void kill() noexcept;

        /** Live updates once per block (pitch, gain, pan, far edge of region). */
        void update (const Settings&) noexcept;

        bool   isActive()    const noexcept { return active; }
        bool   isReleasing() const noexcept { return releasing; }
        double getPosition() const noexcept { return position; }
        const SampleBuffer* getBuffer() const noexcept { return buffer.get(); }

        /** Adds into `out`. Returns the voice's peak in this span (for lamps). */
        float renderAdding (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept;

        static float hermite (const float* data, int length, double pos) noexcept;

    private:
        SampleBuffer::Ptr buffer;
        double hostRate = 44100.0;

        bool   active    = false;
        bool   releasing = false;
        bool   reverse   = false;
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
