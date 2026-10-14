#pragma once

#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Core/EngineBridge.h"

class TwistedBreaksProcessor;

namespace tb
{
    /*
        Resampler  (Phase 12: RESAMPLE)
        ---------
        REC: the engine records the master (or one group) for 1 / 2 / 4 / 8
        bars, starting on the next bar line (or straight away). When the take
        is in, this (message thread):

          1. copies it out of the engine's pre-allocated buffer,
          2. writes it as a 24-bit WAV in the resample folder (background
             thread) - so the set reloads it like any other sample,
          3. loads it onto the target pad (the selected pad when REC was
             pressed), as a tempo-synced loop if AS LOOP is on, and
          4. AUTO SLICEs it if AUTO SLICE is on.

        Folder: Music / Twisted Breaks / Resamples (tests set their own).
    */
    class Resampler final : private juce::Timer
    {
    public:
        explicit Resampler (TwistedBreaksProcessor&);
        ~Resampler() override;

        enum class State { idle, armed, recording, saving };

        void record();
        void cancel();
        State getState() const;
        float getProgress() const;                 // 0..1 while recording
        int   getTargetPad() const noexcept     { return busyPad >= 0 ? busyPad : selectedPad; }
        int   getCount() const noexcept         { return count; }
        juce::File getLastFile() const          { return lastFile; }

        void setSelectedPad (int p) noexcept    { selectedPad = juce::jlimit (0, kNumPads - 1, p); }
        void setFolder (const juce::File& f)    { folder = f; }
        juce::File getFolder() const            { return folder; }

        /** Timer work, public so tests can drive it. */
        void poll();

    private:
        void timerCallback() override { poll(); }
        void saved (const juce::File&, int pad, bool ok);

        TwistedBreaksProcessor& processor;
        juce::File folder, lastFile;
        int  selectedPad = 0, busyPad = -1, slicePad = -1, count = 0;
        bool saving = false, lastRec = false, wantLoop = true, wantSlice = true;

        JUCE_DECLARE_WEAK_REFERENCEABLE (Resampler)
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Resampler)
    };
}
