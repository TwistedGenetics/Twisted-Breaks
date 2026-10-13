#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <vector>

namespace tb
{
    /*
        SampleBuffer
        ------------
        One decoded audio file, IMMUTABLE once built. Created on the loader
        thread, shared by the message thread (UI / waveform) and the audio
        thread (voices) through reference counting.

        Lifetime rule (the reason this is safe on the audio thread):
        SampleManager keeps every buffer it creates in a keep-alive list and
        only drops it on the message thread once nothing else holds a
        reference. So the audio thread can copy and drop SampleBuffer::Ptr
        freely — the count never reaches zero there, so it never frees memory.

        Audio stays at the file's own sample rate; voices compensate in
        their playback increment, so loading never resamples.
    */
    struct SampleBuffer final : juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<SampleBuffer>;

        SampleBuffer()           { counter().fetch_add (1); }
        ~SampleBuffer() override { counter().fetch_sub (1); }

        /** How many buffers exist right now (diagnostics / leak tests). */
        static int liveInstances() noexcept { return counter().load(); }

        juce::AudioBuffer<float> audio;          // 1 or 2 channels
        double       sourceSampleRate = 44100.0;
        juce::String name;                       // file name without extension
        juce::File   file;
        juce::Time   fileModTime;                // to tell if the file changed on disk
        juce::int64  fileSize = 0;

        /** Min/max peaks per bucket of samples (all channels), for drawing. */
        static constexpr int overviewBucket = 256;
        std::vector<float> overviewMin, overviewMax;

        int    getNumSamples()  const noexcept { return audio.getNumSamples(); }
        int    getNumChannels() const noexcept { return audio.getNumChannels(); }
        double getLengthSeconds() const noexcept { return (double) getNumSamples() / sourceSampleRate; }

        void buildOverview()
        {
            const int n       = getNumSamples();
            const int buckets = (n + overviewBucket - 1) / overviewBucket;
            overviewMin.assign ((size_t) buckets, 0.0f);
            overviewMax.assign ((size_t) buckets, 0.0f);

            for (int b = 0; b < buckets; ++b)
            {
                const int s0 = b * overviewBucket;
                const int s1 = juce::jmin (n, s0 + overviewBucket);
                float lo = 0.0f, hi = 0.0f;
                for (int ch = 0; ch < getNumChannels(); ++ch)
                {
                    const auto range = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch, s0), s1 - s0);
                    lo = juce::jmin (lo, range.getStart());
                    hi = juce::jmax (hi, range.getEnd());
                }
                overviewMin[(size_t) b] = lo;
                overviewMax[(size_t) b] = hi;
            }
        }

        /** Min/max over [s0, s1) — uses the overview when the span is wide. */
        juce::Range<float> peakRange (int s0, int s1) const noexcept
        {
            s0 = juce::jlimit (0, getNumSamples(), s0);
            s1 = juce::jlimit (s0, getNumSamples(), s1);
            if (s1 <= s0) return {};

            float lo = 0.0f, hi = 0.0f;
            if (s1 - s0 >= overviewBucket * 2)
            {
                const int b0 = s0 / overviewBucket;
                const int b1 = juce::jmin ((int) overviewMin.size(), (s1 + overviewBucket - 1) / overviewBucket);
                for (int b = b0; b < b1; ++b)
                {
                    lo = juce::jmin (lo, overviewMin[(size_t) b]);
                    hi = juce::jmax (hi, overviewMax[(size_t) b]);
                }
            }
            else
            {
                for (int ch = 0; ch < getNumChannels(); ++ch)
                {
                    const auto r = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch, s0), s1 - s0);
                    lo = juce::jmin (lo, r.getStart());
                    hi = juce::jmax (hi, r.getEnd());
                }
            }
            return { lo, hi };
        }

    private:
        static std::atomic<int>& counter() noexcept { static std::atomic<int> n { 0 }; return n; }
    };
}
