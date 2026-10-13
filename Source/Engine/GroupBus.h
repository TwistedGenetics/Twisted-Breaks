#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace tb
{
    /*
        GroupBus
        --------
        One of the four group buses (A..D). Pads routed to the group render
        their voices into its input buffer; the bus then runs its FX chain
        (Phase 6 — the hook is here, skipped when FX BYPASS is on) and mixes
        into the master with its own level and pan:

            pads -> GroupBus input -> [FX chain] -> level / pan / mute -> master

        Works on spans of any length up to the capacity given in prepare()
        (AudioEngine splits longer spans), so it never allocates and doesn't
        care how the host or the MIDI events cut up a block.

        Level, pan and mute / solo glide over 20 ms, so automating them or
        muting a group mid-break never clicks.
    */
    class GroupBus
    {
    public:
        void prepare (double sampleRate, int maxSpan);
        void reset() noexcept;

        int  getCapacity() const noexcept { return capacity; }

        /** Once per block. audible = not muted and (no group soloed, or this one is). */
        void setParams (float levelDb, float pan, bool audible, bool fxBypass) noexcept;

        /** Clears the first `numSamples` of the input; voices then add into it. */
        juce::AudioBuffer<float>& beginSpan (int numSamples) noexcept;

        /** Something was rendered into the input this span. */
        void markUsed() noexcept { used = true; }

        /** FX, then level / pan, adding into `dest` at `destStart`. Returns the post-fader peak. */
        float mixInto (juce::AudioBuffer<float>& dest, int destStart, int numSamples) noexcept;

        bool isFxBypassed() const noexcept { return fxBypass; }

        /** Balance-style pan used by pads and groups: centre = unity on both sides. */
        static void panGains (float gain, float pan, float& left, float& right) noexcept
        {
            pan   = juce::jlimit (-1.0f, 1.0f, pan);
            left  = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
            right = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
        }

    private:
        juce::AudioBuffer<float> input;
        int  capacity = 0;
        int  spanLength = 0;
        bool used = false;
        bool fxBypass = false;
        bool jumpToParams = true;   // first setParams() after prepare / reset: no glide

        juce::SmoothedValue<float> gainL, gainR;
    };
}
