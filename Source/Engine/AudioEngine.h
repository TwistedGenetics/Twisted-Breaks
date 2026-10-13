#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <vector>
#include "Core/EngineBridge.h"
#include "Core/TransportSync.h"
#include "Engine/PadVoice.h"
#include "Engine/SampleBuffer.h"
#include "Engine/SliceTable.h"
#include "Engine/MasterSection.h"
#include "Engine/GroupBus.h"
#include "Core/FxDefs.h"

namespace tb
{
    /*
        AudioEngine
        -----------
        Owns all real-time DSP. PluginProcessor::processBlock() is a thin
        wrapper that snapshots parameters and calls AudioEngine::process().

        Signal flow (Phase 5):
            MIDI / UI pad events -> per-pad PadVoices
                -> GroupBus A..D (by each pad's GROUP) -> [FX chain, Phase 6]
                -> group level / pan / mute / solo -> master -> output

        Real-time rules enforced here:
          * every buffer is allocated in prepare(), never in process()
          * no locks; UI communication only through EngineBridge
          * samples and slice tables arrive through a lock-free queue (postCommand) already
            decoded; the engine never touches files
          * MIDI is rendered sample-accurately by splitting the block at
            each event's sample position

        SWAP groups (0.4.1)
        -------------------
        Pads in the same SWAP group (1..4) take turns: one plays at a time,
        latched (note-off is ignored), and hitting another pad in the group
        QUEUES a hand-off instead of cutting straight away. The hand-off lands
        at the next slice marker of the break that's playing (or the next
        1/16, beat or bar — the incoming pad's SWAP AT setting), sample-
        accurately, even in the middle of a block:

            amen (pad 1) ... kick . hat . SNARE <- hit pad 2 here
                                          |
            think (pad 2)                 SNARE . hat . kick ...

        The incoming break carries on from the SAME musical point: a synced
        loop takes its own grid position for that song position, otherwise
        the same fraction of the way through; then it snaps to its own
        nearest slice marker (within a 1/32 note) so it starts on its hit,
        not a few ms into it. Hitting the playing pad again stops it at the
        next point. Everything is plain data on the audio thread.
    */
    class AudioEngine
    {
    public:
        static constexpr int kVoicesPerPad = 2;  // current + one fading out on retrigger

        explicit AudioEngine (EngineBridge& bridge);
        ~AudioEngine();

        void prepare (double sampleRate, int maxBlockSize);
        void reset() noexcept;

        struct PadParams
        {
            float gainDb  = 0.0f;
            float pan     = 0.0f;
            float pitch   = 0.0f;
            float start   = 0.0f;
            float end     = 1.0f;
            bool  reverse = false;
            int   mode    = 0;       // params::PadMode
            bool  sync    = true;
            int   rate    = 2;       // index into params::loopRates
            int   choke   = 0;       // 0 = off, 1..4
            float velSens = 1.0f;
            bool  mute    = false;
            bool  solo    = false;
            int   slice   = 1;       // 1..16, SLICE mode
            int   swap    = 0;       // 0 = off, 1..4: SWAP group
            int   swapAt  = 0;       // params::SwapAt: when a hand-off lands
            int   group   = 0;       // 0..3 = A..D
            bool  fixVel  = false;   // every hit at full level
            bool  sliceSeq  = false; // a hit starts / stops the slice sequence
            int   sliceSlot = 0;     // 0..7
        };

        struct GroupParams
        {
            float levelDb = 0.0f;
            float pan     = 0.0f;
            bool  mute    = false;
            bool  solo    = false;
            bool  bypass  = false;
            std::array<bool, 16> fxOn {};                       // fx::Id order
            std::array<std::array<float, 3>, 16> fxParam {};    // raw values (0..1 or choice index)
        };

        struct BlockParams
        {
            float masterGainDb   = 0.0f;
            bool  limiterEnabled = true;
            bool  panicParam     = false;
            std::array<PadParams, kNumPads> pads {};
            std::array<GroupParams, kNumGroups> groups {};

            bool  seqRun    = true;
            int   seqLength = 16;
            int   seqRes    = 1;     // index into params::seqResPpq
            int   seqQuant  = 2;     // index into params::seqQuantPpq
        };

        void process (juce::AudioBuffer<float>& buffer,
                      const juce::MidiBuffer& midi,
                      const TransportState& transport,
                      const BlockParams& params) noexcept;

        /** Stops every voice with a short fade. Safe on the audio thread. */
        void panic() noexcept;

        // ---- Sample hand-off (message thread -> audio thread) ----------------
        /**
            One change to a pad's content. `buffer` (may be nullptr = clear)
            must carry one extra reference that the engine adopts.
        */
        struct PadCommand
        {
            int pad = 0;
            bool setSample = false;
            SampleBuffer* buffer = nullptr;
            bool setSlices = false;
            SliceTable slices;
        };

        /** Returns false if the queue is full (caller retries later). */
        bool postCommand (const PadCommand&) noexcept;

        /** Message thread, audio stopped: drop anything still queued. */
        void drainSampleQueue() noexcept;

        // ---- MIDI note map (pads default to notes 36..51) ---------------------
        void setPadNote (int pad, int note) noexcept;
        int  getPadNote (int pad) const noexcept;
        int  padForNote (int note) const noexcept;   // -1 if unmapped

        /** MIDI learn: the next note-on received is assigned to `pad` (-1 cancels). */
        void startNoteLearn (int pad) noexcept    { learnPad.store (pad); }
        int  getNoteLearnPad() const noexcept     { return learnPad.load(); }
        /** Bumps whenever the note map changes (UI polls it to mark the state dirty). */
        uint32_t getNoteMapVersion() const noexcept { return noteMapVersion.load(); }

        static constexpr int kDefaultFirstNote = 36;  // C1 in Ableton = Drum Rack pad 1
        static constexpr int kSwapGroups = 4;

        // ---- Performance FX from MIDI (notes C3..D#4 = FX 1..16) --------------
        /** Group the FX notes (and the FX grid) act on: 0..3. */
        void setFxTargetGroup (int g) noexcept   { fxTargetGroup.store (juce::jlimit (0, kNumGroups - 1, g)); }
        int  getFxTargetGroup() const noexcept   { return fxTargetGroup.load(); }
        /** Bit per fx::Id: 1 = LOCKED (a note toggles), 0 = MOMENTARY (on while held). */
        void setFxLockedMask (uint32_t m) noexcept { fxLocked.store (m); }
        uint32_t getFxLockedMask() const noexcept  { return fxLocked.load(); }
        /** UI: clear MIDI latches so the on/off parameters alone decide. */
        void clearFxLatch (int group, int fxId) noexcept;

    private:
        void triggerPad (int pad, float velocity, int sampleOffset, bool fromSequencer = false) noexcept;
        void startVoice (int pad, float level, int sampleOffset, double startAt = -1.0,
                         const PadVoice::Settings* custom = nullptr) noexcept;
        void startPad (int pad, float level, int sampleOffset, double startAt = -1.0) noexcept;
        float levelFor (int pad, float velocity) const noexcept;

        // ---- SWAP groups
        struct PendingSwap
        {
            bool  active    = false;
            int   pad       = -1;     // incoming pad; -1 = stop the group
            float level     = 1.0f;
            int   remaining = 0;      // output samples until it lands
        };
        int    swapOwner (int group) const noexcept;           // playing pad, or -1
        int    samplesUntilSwap (int owner, int swapAt, int sampleOffset) const noexcept;
        double handoffPosition (int fromPad, int toPad, int sampleOffset) const noexcept;
        void   fireSwap (int group, int sampleOffset) noexcept;
        void chokeOthers (int pad, int chokeGroup) noexcept;
        void releasePad (int pad) noexcept;
        void handleMidi (const juce::MidiMessage& m) noexcept;
        void renderVoices (juce::AudioBuffer<float>& buffer, int start, int num) noexcept;
        void renderSpan (juce::AudioBuffer<float>& buffer, int start, int num) noexcept;
        bool handleFxNote (int note, bool on) noexcept;

        // ---- Sequencer
        enum class SeqKind : uint8_t { padHit, sliceRelease, sliceHit };   // order = priority on a tie
        struct SeqEvent { int offset; int pad; float velocity; SeqKind kind; uint64_t step; uint32_t gen; };
        void collectSequencerEvents (const TransportState&, const BlockParams&, int numSamples) noexcept;
        void insertEvent (const SeqEvent&) noexcept;

        // ---- Slice sequencer
        struct SliceRun
        {
            bool     running = false;
            int64_t  anchor = 0;       // absolute step that is pattern step 0
            int      slot = 0;
            int      lastSlice = 0;
            uint32_t gen = 0;          // bumps on start / stop: stale events are ignored
            double   stepPpq = 0.25;   // current slot's step size
        };
        void startSliceSeq (int pad, int sampleOffset) noexcept;
        void stopSliceSeq (int pad) noexcept;
        void addSliceEvents (int pad, int fromOffset) noexcept;
        void sliceHit (int pad, const SliceStep&, float velocity, int sampleOffset) noexcept;
        /** The slot's pattern (or its default if never written) and its settings. */
        int  slicePattern (int pad, int slot, std::array<SliceStep, SliceSeqData::kSteps>&, SliceSlot&) const noexcept;
        double clockAt (int sampleOffset) const noexcept { return clock0 + sampleOffset * clockDppq; }
        void recordHit (int pad, float velocity, int sampleOffset) noexcept;
        void applyPadCommands() noexcept;
        void computeVoiceSettings (const BlockParams&) noexcept;

        EngineBridge& bridge;
        MasterSection master;

        SpscQueue<PadCommand, 128> padCommands;
        std::array<SliceTable, kNumPads> padSlices {};

        std::array<SampleBuffer::Ptr, kNumPads> padSample;           // audio-thread owned
        std::array<PadVoice::Settings, kNumPads> padSettings {};     // this block's settings
        std::array<std::array<PadVoice, kVoicesPerPad>, kNumPads> voices;
        std::array<int, kNumPads>   currentVoice {};
        std::array<float, kNumPads> padPeak {};       // per-block peak for lamps
        std::array<bool, kNumPads>  padFlash {};      // triggered this block
        std::array<std::atomic<int>, kNumPads> padNote {};
        std::array<int,  kNumPads>  padMode {};       // this block's mode per pad
        std::array<int,  kNumPads>  padChoke {};
        std::array<float, kNumPads> padVelSens {};
        std::array<bool,  kNumPads> padFixVel {};
        std::array<int,  kNumPads>  padSwap {};       // 0 = off, 1..4
        std::array<int,  kNumPads>  padGroup {};      // 0..3 = A..D
        std::array<GroupBus, kNumGroups> groups;
        std::array<float, kNumGroups> groupPeak {};   // per-block, for the mixer meters

        // FX state from MIDI: latched (LOCKED notes) and held (MOMENTARY notes).
        std::array<std::atomic<uint32_t>, kNumGroups> fxLatch {};
        std::array<uint32_t, kNumGroups> fxHeld {};
        std::array<uint32_t, kNumGroups> fxParamMask {};
        std::array<std::array<std::array<float, 3>, 16>, kNumGroups> fxParams {};
        std::atomic<int>      fxTargetGroup { 0 };

        static constexpr int kMaxSeqEvents = 1024;
        std::array<SeqEvent, kMaxSeqEvents> seqEvents {};
        int seqEventCount = 0;
        int seqCursor = 0;          // next event to play; inserts never go before it
        std::array<int64_t, kNumPads> seqSkipOnce {};    // absolute step just live-recorded: don't double it this pass
        std::array<SliceRun, kNumPads> sliceRun {};
        std::array<bool, kNumPads> padSliceSeq {};
        std::array<int,  kNumPads> padSliceSlot {};
        std::array<float, kNumPads> padPitchNow {};
        double freePpq = 0.0, clock0 = 0.0, clockDppq = 0.0;   // slice-sequencer clock (Live's, or free when stopped)
        bool   clockPlaying = false;
        int    blockLength = 0;
        juce::Random seqRandom { 4242 };
        int    seqLengthNow = 16;
        double seqStepPpq = 0.25, seqQuantPpq = 0.25;
        bool   seqRecording = false;                     // transport running: hits may be recorded
        std::atomic<uint32_t> fxLocked { fx::kDefaultLockedMask };
        std::array<int,  kNumPads>  padSwapAt {};
        std::array<int,  kSwapGroups + 1> swapPlaying {};   // [1..4]: pad, or -1
        std::array<PendingSwap, kSwapGroups + 1> pendingSwap {};
        std::atomic<int>      learnPad { -1 };
        std::atomic<uint32_t> noteMapVersion { 0 };
        const TransportState* transportNow = nullptr; // valid during process()
        int currentOffset = 0;                         // sample offset of the event being handled

        bool   lastPanicParam = false;
        double sampleRate     = 44100.0;

        // Scope decimation
        int   scopeDecimation = 8;
        int   scopeCounter    = 0;
        std::vector<float> scopeScratch;              // sized in prepare()
    };
}
