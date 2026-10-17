#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include "Core/TransportSync.h"
#include "Core/SequencerData.h"
#include "Core/SliceSeqData.h"

namespace tb
{
    /** Phase 13: two scenes of 16 pads. Engine pad p = scene * 16 + position. */
    inline constexpr int kPadsPerScene = 16;
    inline constexpr int kNumScenes    = 2;
    inline constexpr int kNumPads      = kPadsPerScene * kNumScenes;   // 32
    inline constexpr int kNumGroups    = 4;
    inline constexpr uint32_t kAllPadsMask = 0xffffffffu;
    static_assert (kNumPads <= 32, "pad masks are 32-bit");
    static_assert (SequencerData::kPads == kNumPads && SliceSeqData::kPads == kNumPads, "sequencer lanes = pads");
    inline constexpr int sceneOf (int pad) noexcept { return pad / kPadsPerScene; }
    /** "01".."16" for scene A, "B01".."B16" for scene B. */
    inline juce::String padLabel (int pad)
    {
        return (sceneOf (pad) == 1 ? "B" : "") + juce::String (pad % kPadsPerScene + 1).paddedLeft ('0', 2);
    }

    /*
        Single-producer / single-consumer lock-free queue of trivially
        copyable items, backed by a fixed array (no allocation after
        construction). Used for every message that crosses between the
        message (UI) thread and the audio thread.
    */
    template <typename T, int Capacity>
    class SpscQueue
    {
    public:
        bool push (const T& item) noexcept
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 + n2 == 0) return false;            // full: drop, never block
            buffer[(size_t) (n1 > 0 ? s1 : s2)] = item;
            fifo.finishedWrite (1);
            return true;
        }

        bool pop (T& item) noexcept
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 + n2 == 0) return false;
            item = buffer[(size_t) (n1 > 0 ? s1 : s2)];
            fifo.finishedRead (1);
            return true;
        }

        void clear() noexcept { fifo.reset(); }

    private:
        juce::AbstractFifo fifo { Capacity };
        std::array<T, (size_t) Capacity> buffer {};
    };

    /** A pad press/release coming from the UI (mouse) rather than MIDI. */
    struct PadUiEvent
    {
        int   pad      = 0;
        float velocity = 1.0f;
        bool  noteOn   = true;
    };

    /** A8: what the Mikro MK3 asks of the engine (its USB thread is the only producer). */
    struct ControllerHit
    {
        enum Kind : uint8_t { padOn, padOff, sliceOn, rollOn, rollOff, sliceOff };   // sliceOff: only for MIDI out (A12)
        Kind    kind = padOn;
        int8_t  pad = 0;
        uint8_t slice = 0;       // 1..16 (slice hits, rolls of a slice)
        uint8_t rate = 1;        // rolls: 0 = 1/8, 1 = 1/16, 2 = 1/32
        float   velocity = 1.0f;     // rollOn: > 0 = with a hit just now; < 0 = a pad already held (|velocity|)
    };

    /*
        EngineBridge
        ------------
        Everything the UI and audio threads share, in one place, all of it
        either atomic or lock-free. The audio thread never waits on the UI.

          UI -> audio : padEvents, panicRequested
          audio -> UI : pad activity, MIDI activity, meters, scope, transport
    */
    struct EngineBridge
    {
        // ---- UI -> audio -----------------------------------------------------
        SpscQueue<PadUiEvent, 256> padEvents;
        /** A7: pads played from a controller's own port (Push 3 User mode). Producer: that port's MIDI thread. */
        SpscQueue<PadUiEvent, 256> controllerPadEvents;
        /** A7: Push 3 pad pressure per pad, 0..1 (MIDI thread writes, audio reads). */
        std::array<std::atomic<float>, kNumPads> padPressure {};
        /** A8: Mikro MK3 pads, slices and rolls. Producer: the Mikro's USB thread. */
        SpscQueue<ControllerHit, 256> mikroEvents;
        std::atomic<bool>          panicRequested { false };

        // ---- audio -> UI -----------------------------------------------------
        std::array<std::atomic<float>, kNumPads>    padActivity {};  // 0..1 envelope for lamps
        std::array<std::atomic<uint32_t>, kNumPads> padTriggerCount {};
        std::array<std::atomic<float>, kNumPads>    padPlayhead {};  // 0..1 of sample, < 0 = idle
        std::array<std::atomic<bool>, kNumPads>     padQueued {};    // SWAP hand-off waiting to land
        std::array<std::atomic<bool>, kNumPads>     padLoaded {};    // pad has a playable sample

        // ---- Sequencer (shared: UI edits, audio plays + records) ----------------
        SequencerData    seq;
        std::atomic<int> seqStep { -1 };            // step under the playhead, -1 = stopped
        std::atomic<int> programChange { 0 };       // MIDI Program Change + 1 (snapshot recall), 0 = none

        // ---- Slice sequencer (per pad) ---------------------------------------------
        SliceSeqData sliceSeq;
        std::array<std::atomic<int>, kNumPads>  sliceStep {};    // pattern step playing, -1 = not running
        std::array<std::atomic<int>, kNumPads>  laneStep {};     // A6: a launched SEQ pad's step, -1 = not running
        std::array<std::atomic<int>, kNumPads>  sliceSlotNow {}; // slot actually playing (0..7)
        std::array<std::atomic<bool>, kNumPads> sliceFill {};    // FILL held (UI -> audio)

        std::atomic<int>      lastMidiNote     { -1 };

        // ---- A12: MIDI out of what you play (for recording into Live) ----------------
        std::atomic<bool> midiOutEnabled { true };    // anything goes out at all (plugin out / port)
        std::atomic<bool> midiOutSteps   { false };   // main-sequencer steps too
        std::atomic<bool> midiSliceChannel { true };  // channel 2 notes 36-51 in = slices of the selected pad
        std::atomic<int>  selectedPad    { 0 };       // the window / controller selection (channel-2 slices)
        std::atomic<int>      lastMidiVelocity { 0 };
        std::atomic<uint32_t> midiEventCount   { 0 };

        std::atomic<float> peakLeft  { 0.0f };   // linear, held by the UI
        std::atomic<float> peakRight { 0.0f };
        std::array<std::atomic<float>, kNumGroups> groupPeak {};   // post-fader, linear, held by the UI
        std::array<std::atomic<uint32_t>, kNumGroups> fxEngaged {}; // effects audibly on (bit per fx::Id)
        std::array<std::atomic<uint32_t>, kNumGroups> fxArmed {};   // effects waiting for their boundary
        std::atomic<uint32_t> macroActive { 0 };                     // performance macros on (bit per macro)
        std::atomic<float> limiterReductionDb { 0.0f };
        std::atomic<uint32_t> clipCount { 0 };   // increments on any over

        TransportUiState transport;

        // Oscilloscope tap (decimated mono master). Audio writes, UI reads.
        static constexpr int scopeCapacity = 4096;
        juce::AbstractFifo scopeFifo { scopeCapacity };
        std::array<float, (size_t) scopeCapacity> scopeData {};

        void pushScope (const float* samples, int num) noexcept
        {
            int s1, n1, s2, n2;
            fifo_write (samples, num, s1, n1, s2, n2);
        }

        int pullScope (float* dest, int maxNum) noexcept
        {
            int s1, n1, s2, n2;
            scopeFifo.prepareToRead (maxNum, s1, n1, s2, n2);
            std::copy_n (scopeData.data() + s1, n1, dest);
            std::copy_n (scopeData.data() + s2, n2, dest + n1);
            scopeFifo.finishedRead (n1 + n2);
            return n1 + n2;
        }

    private:
        void fifo_write (const float* samples, int num, int& s1, int& n1, int& s2, int& n2) noexcept
        {
            scopeFifo.prepareToWrite (num, s1, n1, s2, n2);
            std::copy_n (samples, n1, scopeData.data() + s1);
            std::copy_n (samples + n1, n2, scopeData.data() + s2);
            scopeFifo.finishedWrite (n1 + n2);   // if the UI is slow, excess is simply dropped
        }
    };
}
