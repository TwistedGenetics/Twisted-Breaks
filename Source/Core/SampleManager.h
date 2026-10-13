#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <array>
#include <vector>
#include "Core/EngineBridge.h"
#include "Engine/SampleBuffer.h"
#include "Engine/SampleLoader.h"

namespace tb
{
    class AudioEngine;

    /*
        SampleManager  (message thread)
        -------------
        Owns which sample each pad has, and moves samples safely to the
        audio thread:

          1. loadFile()        -> decode job on the background loader thread
          2. job finishes      -> result posted back to the message thread
          3. install()         -> buffer kept alive here, then queued to the
                                  engine lock-free (AudioEngine::postSample)
          4. timer (20 Hz)     -> retries a full queue; frees buffers nothing
                                  references any more — always here, never on
                                  the audio thread

        Also the source of truth for sample paths in saved state, and for the
        pad status the UI shows (empty / loading / ready / missing / error).
    */
    class SampleManager final : private juce::Timer
    {
    public:
        enum class Status { empty, loading, ready, missing, unreadable, tooLong };

        struct PadInfo
        {
            Status            status = Status::empty;
            juce::String      name;        // display name (file name, no extension)
            juce::File        file;        // what was asked for (kept when missing)
            SampleBuffer::Ptr buffer;      // only when ready
            uint32_t          version = 0; // bumps on every change, for UI polling
        };

        explicit SampleManager (AudioEngine&);
        ~SampleManager() override;

        /** Message thread. Decodes in the background; the pad shows LOADING until done. */
        void loadFile (int pad, const juce::File&);
        void clearPad (int pad);

        PadInfo  getInfo (int pad) const;
        uint32_t getVersion (int pad) const;
        bool     isBusy() const;          // any pad still loading (used by tests)

        static bool isSupportedFile (const juce::File& f) { return SampleLoader::isSupportedFile (f); }
        static juce::String statusText (Status);

        // ---- State ------------------------------------------------------------
        /** <PADS><PAD index="0" path="..."/>...</PADS>  — callable from any thread. */
        juce::ValueTree toValueTree() const;
        /** Restores paths and starts loading them. Hops to the message thread if needed. */
        void restoreFromValueTree (const juce::ValueTree&);

        static const juce::Identifier padsId;

    private:
        void timerCallback() override;
        void install (int pad, uint32_t generation, const SampleLoader::Result&);
        void queueForEngine (int pad, const SampleBuffer::Ptr&);
        void flushPending();
        void setInfo (int pad, PadInfo&& info);

        AudioEngine& engine;
        juce::AudioFormatManager formats;
        juce::ThreadPool loader;

        mutable juce::CriticalSection lock;            // guards `pads` (never taken by audio)
        std::array<PadInfo, kNumPads>  pads;
        std::array<uint32_t, kNumPads> generation {};  // discards stale load results

        juce::ReferenceCountedArray<SampleBuffer> keepAlive;
        struct Pending { int pad; SampleBuffer::Ptr buffer; };
        std::vector<Pending> pending;

        JUCE_DECLARE_WEAK_REFERENCEABLE (SampleManager)
        JUCE_DECLARE_NON_COPYABLE (SampleManager)
    };
}
