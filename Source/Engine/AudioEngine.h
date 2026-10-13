#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include "Core/EngineBridge.h"
#include "Core/TransportSync.h"
#include "Engine/CalibrationVoice.h"
#include "Engine/MasterSection.h"

namespace tb
{
    /*
        AudioEngine
        -----------
        Owns all real-time DSP. PluginProcessor::processBlock() is a thin
        wrapper that reads parameters and calls AudioEngine::process().

        Signal flow (Phase 1):
            MIDI / UI pad events -> per-pad voices -> master -> output
        Signal flow (from Phase 5):
            voices -> GroupBus A..D -> FXChain per group -> master -> output

        Real-time rules enforced here:
          * every buffer is allocated in prepare(), never in process()
          * no locks; UI communication only through EngineBridge
          * MIDI is rendered sample-accurately by splitting the block at
            each event's sample position
    */
    class AudioEngine
    {
    public:
        static constexpr int kVoicesPerPad = 2;  // current + one fading out on retrigger

        explicit AudioEngine (EngineBridge& bridge);

        void prepare (double sampleRate, int maxBlockSize);
        void reset() noexcept;

        struct BlockParams
        {
            float masterGainDb   = 0.0f;
            bool  limiterEnabled = true;
            bool  panicParam     = false;
        };

        void process (juce::AudioBuffer<float>& buffer,
                      const juce::MidiBuffer& midi,
                      const TransportState& transport,
                      const BlockParams& params) noexcept;

        /** Stops every voice with a short fade. Safe on the audio thread. */
        void panic() noexcept;

        // ---- MIDI note map (pads default to notes 36..51) ---------------------
        // Atomic so the UI can remap at any time without locking audio.
        void setPadNote (int pad, int note) noexcept;
        int  getPadNote (int pad) const noexcept;
        int  padForNote (int note) const noexcept;   // -1 if unmapped

        static constexpr int kDefaultFirstNote = 36;  // C1 in Ableton = Drum Rack pad 1

    private:
        void triggerPad (int pad, float velocity) noexcept;
        void releasePad (int pad) noexcept;
        void handleMidi (const juce::MidiMessage& m) noexcept;
        void renderVoices (juce::AudioBuffer<float>& buffer, int start, int num) noexcept;

        EngineBridge& bridge;
        MasterSection master;

        std::array<std::array<CalibrationVoice, kVoicesPerPad>, kNumPads> voices;
        std::array<int, kNumPads>   currentVoice {};
        std::array<float, kNumPads> padPeak {};       // per-block peak for lamps
        std::array<std::atomic<int>, kNumPads> padNote {};

        bool   lastPanicParam = false;
        double sampleRate     = 44100.0;

        // Scope decimation
        int   scopeDecimation = 8;
        int   scopeCounter    = 0;
        std::vector<float> scopeScratch;              // sized in prepare()
    };
}
