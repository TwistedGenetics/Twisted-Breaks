#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <array>
#include <functional>
#include <vector>
#include "Core/EngineBridge.h"
#include "Engine/SampleBuffer.h"
#include "Engine/SampleLoader.h"
#include "Engine/SliceTable.h"

namespace tb
{
    class AudioEngine;

    /*
        SampleManager  (message thread)
        -------------
        Owns what each pad plays — its sample and its slice markers — and
        moves that safely to the audio thread:

          1. loadFile() / autoSlice()  -> work on the background loader thread
          2. result posted back to the message thread
          3. buffer kept alive here, then queued to the engine lock-free
          4. timer (20 Hz) retries a full queue and frees buffers nothing
             references any more — always here, never on the audio thread

        Pads that use the same file share ONE decoded copy (SPREAD, or a set
        reopened with sixteen slices of the same break), so memory stays flat.

        Also the source of truth for saved state (paths, slices, slicer
        settings) and for the status the UI shows.
    */
    class SampleManager final : private juce::Timer
    {
    public:
        enum class Status { empty, loading, ready, missing, unreadable, tooLong };

        struct PadInfo
        {
            Status            status = Status::empty;
            juce::String      name;          // display name (file name, no extension)
            juce::File        file;          // what was asked for (kept when missing)
            SampleBuffer::Ptr buffer;        // only when ready
            SliceTable        slices;        // current markers (may be empty)
            SliceTable        detected;      // what RESET goes back to
            float             sensitivity = 0.5f;
            int               sliceCount  = 16;   // AUTO keeps at most this many; EQUAL makes this many
            bool              slicing = false;    // AUTO SLICE running
            uint32_t          version = 0;        // bumps on every change, for UI polling
        };

        explicit SampleManager (AudioEngine&);
        ~SampleManager() override;

        // ---- Samples (message thread) -----------------------------------------
        void loadFile (int pad, const juce::File&);
        void clearPad (int pad);

        // ---- Slicing (message thread) ------------------------------------------
        void autoSlice (int pad);                         // transient detection, background
        void equalSlice (int pad);
        void resetSlices (int pad);                       // back to the last AUTO / EQUAL result
        void clearSlices (int pad);
        void setSlices (int pad, SliceTable);             // normalised, then sent to the engine
        bool addMarker (int pad, int position);           // false when 16 already
        void moveMarker (int pad, int index, int position);
        void deleteMarker (int pad, int index);
        void setSensitivity (int pad, float);
        void setSliceCount (int pad, int);
        /** Copies the source pad's sample + markers onto the following pads
            (shared, not re-decoded). Returns how many pads were filled. */
        int  spreadToPads (int sourcePad);

        PadInfo  getInfo (int pad) const;
        uint32_t getVersion (int pad) const;
        bool     isBusy() const;          // loading or slicing (used by tests)

        static bool isSupportedFile (const juce::File& f) { return SampleLoader::isSupportedFile (f); }
        static juce::String statusText (Status);

        // ---- State ------------------------------------------------------------
        /** <PADS><PAD index="0" path="..." slices="0,5512,..." .../></PADS> — any thread. */
        juce::ValueTree toValueTree() const;
        /** Restores paths + slices and starts loading. Hops to the message thread if needed. */
        void restoreFromValueTree (const juce::ValueTree&);

        static const juce::Identifier padsId;

        /** Called on the message thread whenever a pad's content or status changes. */
        std::function<void()> onStateChanged;

    private:
        struct Pending
        {
            int pad = 0;
            bool setSample = false;
            SampleBuffer::Ptr buffer;
            bool setSlices = false;
            SliceTable slices;
        };

        void timerCallback() override;
        void startLoad (int pad, const juce::File&, const SliceTable* keepSlices);
        void install (int pad, uint32_t generation, const SampleLoader::Result&);
        void installBuffer (int pad, const SampleBuffer::Ptr&, const SliceTable* keepSlices);
        void sendSample (int pad, const SampleBuffer::Ptr&);
        void sendSlices (int pad, const SliceTable&);
        void flushPending();
        void setInfo (int pad, PadInfo&& info);
        void modify (int pad, const std::function<void (PadInfo&)>& change, bool slicesChanged);
        SampleBuffer::Ptr findLoaded (const juce::File&) const;

        AudioEngine& engine;
        juce::AudioFormatManager formats;
        juce::ThreadPool loader;

        mutable juce::CriticalSection lock;            // guards `pads` (never taken by audio)
        std::array<PadInfo, kNumPads>  pads;
        std::array<uint32_t, kNumPads> generation {};  // discards stale load results
        std::array<uint32_t, kNumPads> sliceGeneration {};

        // Restore: slices to re-apply once the sample arrives; pads waiting on
        // another pad that is already decoding the same file.
        std::array<SliceTable, kNumPads> restoreSlices {};
        std::array<SliceTable, kNumPads> restoreDetected {};
        std::array<bool, kNumPads>       hasRestoreSlices {};
        std::array<int, kNumPads>        followLeader {};
        std::array<uint32_t, kNumPads>   followGen {};

        juce::ReferenceCountedArray<SampleBuffer> keepAlive;
        std::vector<Pending> pending;

        JUCE_DECLARE_WEAK_REFERENCEABLE (SampleManager)
        JUCE_DECLARE_NON_COPYABLE (SampleManager)
    };
}
