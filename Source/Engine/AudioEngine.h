#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <vector>
#include <limits>
#include "Core/EngineBridge.h"
#include "Core/TransportSync.h"
#include "Engine/PadVoice.h"
#include "Engine/SampleBuffer.h"
#include "Engine/SliceTable.h"
#include "Engine/MasterSection.h"
#include "Engine/GroupBus.h"
#include "Core/FxDefs.h"
#include "Core/SequencerData.h"
#include "Core/MacroDefs.h"
#include "Core/Parameters.h"

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
            bool  followBank = false; // play the active bank break
            int   character = 0;      // PadVoice::character (vintage mode)
            int   launch    = 2;      // params::Launch (SEQ modes)
            int   seqLen    = 0;      // 0 = global LEN
            int   swapFrom  = 0;      // 0 same step, 1 step 1
            int   press     = 0;      // params::PressTarget (Push 3 pad pressure)
            bool  stepsWhole = false; // A10: steps play the whole pad, never a slice
            bool  fit = false;        // A11: one-shots / slices follow Live's tempo
            int   layerMode = 0;      // A11: params::LayerMode
            int   stretch = 0;        // A12: params::StretchMode as set (see params::effectiveStretch for sync / FIT)
            float origBpm = 0.0f;     // A12: 0 = detect
            int   gap = 1;            // A12: BEATS gap fill
            float grainMs = 30.0f;    // A12: TEXTURE grain / AKAI cycle
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
            bool  seqFill   = false; // FILL held (step conditions)

            int   bankActive = 0;    // 0..7
            int   bankAt     = 1;    // params::SwitchAt

            float    xfade       = 0.5f;  // 0 A .. 1 B
            int      xfadeCurve  = 0;     // params::XfadeCurve
            int      sceneView   = 0;     // 0 A, 1 B: pad notes 36-51 play this scene first

            uint32_t macroOn     = 0;     // bit per macro (parameters)
            float    macroAmount = 0.6f;  // 0..1
            int      macroGroup  = 4;     // 0..3, 4 = all
            bool     macroLatch  = false;
        };

        static constexpr int kBankSlots = 8;
        /** Pad commands with pad = kNumPads + slot fill break-bank slots. */
        static constexpr int kBankCommandBase = kNumPads;
        static constexpr int kPreviewCommand  = -100;     // PadCommand.pad: audition a buffer (preset browser)
        static constexpr int kLayerCommandBase = 1000;    // PadCommand.pad: 1000 + pad * 4 + layer (A11 layers)

        /** Snapshot switch (audio thread, at the boundary): every pad's own sample + slices. */
        void applySnapshotPads (const std::array<SampleBuffer*, kNumPads>& buffers,
                                const std::array<SliceTable, kNumPads>& slices) noexcept;
        int  getActiveBank() const noexcept { return bankActiveNow.load (std::memory_order_relaxed); }

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
        /** Tests: a pad's voices (read only). */
        const auto& voicesForTesting (int pad) const noexcept { return voices[(size_t) juce::jlimit (0, kNumPads - 1, pad)]; }
        int  padForNote (int note) const noexcept;   // -1 if unmapped; the scene on view wins a shared note

        // ---- Scenes (Phase 13) -------------------------------------------------
        /** UI cut keys: -1 = the crossfader decides, 0 = all A, 1 = all B (while held). */
        void setCrossfaderCut (int scene) noexcept { xfadeCut.store (juce::jlimit (-1, 1, scene)); }
        int  getCrossfaderCut() const noexcept     { return xfadeCut.load(); }
        int  getSceneView() const noexcept         { return sceneViewNow.load (std::memory_order_relaxed); }
        float getSceneGain (int scene) const noexcept { return sceneGain[(size_t) juce::jlimit (0, kNumScenes - 1, scene)]; }

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
        /** UI: forget MIDI macro latches (the keys decide). */
        void clearMacroLatch() noexcept { macroMidiLatch.store (0); }
        /** What the group's chain was last told for an effect setting (tests / display). */
        float getAppliedFxParam (int group, int fxId, int param) const noexcept
        {
            return appliedFxParams[(size_t) juce::jlimit (0, kNumGroups - 1, group)][(size_t) juce::jlimit (0, 15, fxId)][(size_t) juce::jlimit (0, 2, param)];
        }

        // ---- RESAMPLE recorder (Phase 12) ------------------------------------
        /*
            Records the master output (source 0) or a group's output (1..4)
            for `bars` bars, from the next bar line (or straight away) into a
            buffer allocated in prepare() (kMaxResampleSeconds). States:
            0 idle, 1 armed, 2 recording, 3 done (the message thread copies
            the take, then calls resampleTaken()).
        */
        static constexpr double kMaxResampleSeconds = 32.0;
        enum ResampleState { rsIdle = 0, rsArmed = 1, rsRecording = 2, rsDone = 3 };
        /** Message thread. False if a take is still being recorded / collected. */
        bool armResample (int source, int bars, bool atBar) noexcept;
        void cancelResample() noexcept           { rsCancel.store (true); }
        int  getResampleState() const noexcept   { return rsState.load(); }
        int  getResampleWritten() const noexcept { return rsWritten.load(); }
        int  getResampleLength() const noexcept  { return rsLength.load(); }
        /** Message thread, in state rsDone: the take (first getResampleLength() samples). */
        const juce::AudioBuffer<float>& getResampleTake() const noexcept { return rsBuffer; }
        void resampleTaken() noexcept            { int d = rsDone; rsState.compare_exchange_strong (d, rsIdle); }
        double getSampleRate() const noexcept    { return sampleRate; }

    private:
        void resampleCapture (const juce::AudioBuffer<float>& master, const TransportState&, int numSamples) noexcept;
        void triggerPad (int pad, float velocity, int sampleOffset, bool fromSequencer = false,
                         const SeqStep* step = nullptr) noexcept;
        /** A step's pitch / reverse / slice, as voice settings for this pad. */
        PadVoice::Settings tweakedSettings (int pad, const SeqStep&) const noexcept;
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
        // Order = priority on a tie. A step FX's ON comes before the previous
        // step's OFF, so the same effect on back-to-back steps runs unbroken.
        enum class SeqKind : uint8_t { bankSwitch, stepFxOn, stepFxOff, laneStop, padHit, sliceRelease, sliceHit, rollHit };
        /** owner >= 0: a launched sequence's event, dropped if that run's generation moved on. */
        struct SeqEvent { int offset; int pad; float velocity; SeqKind kind; uint64_t step; uint32_t gen; int owner = -1; uint32_t ownerGen = 0; };
        void collectSequencerEvents (const TransportState&, const BlockParams&, int numSamples) noexcept;
        void insertEvent (const SeqEvent&) noexcept;

        /** One sequencer step into the event list (main lanes and launched lanes share it).
            `base` / `rate`: the ppq at offset 0 and per sample of the domain `stepStart` is in. */
        struct StepCtx
        {
            double p0, p1, base, rate, stepPpq;
            int numSamples;
            bool fill;
        };
        void emitStep (const StepCtx&, int pad, const SeqStep&, double stepStart, int64_t pass, bool firstPass,
                       int64_t rollKey, int64_t& rollStep, bool& rollPlays, float velScale, int owner, uint32_t ownerGen) noexcept;

        // ---- Sequence pads (A6): a pad in SEQ ONE / SEQ LOOP plays its lane when hit
        struct LaneRun
        {
            bool     running = false, once = false;
            double   anchor  = 0.0;        // clock ppq of step 1: on the song grid (Live's playhead), or the hand-off for FROM STEP 1
            double   start   = 0.0;        // nothing plays before this (the LAUNCH boundary)
            double   endPpq  = 1.0e300;    // this segment is cut here (stop / restart / hand-off)
            bool     releaseAtEnd = false; // a stop: cut the sound at endPpq
            bool     hasNext = false;      // SEQ ONE re-hit: the next segment starts at nextAnchor
            double   nextAnchor = 0.0, nextStart = 0.0;
            float    hitVel = 1.0f;
            uint32_t gen = 0;
            int64_t  rollKey = std::numeric_limits<int64_t>::min();
            bool     rollPlays = false;
        };
        bool   isSeqPad (int pad) const noexcept { return params::isSeqMode (padMode[(size_t) pad]); }
        bool   isContRunning (int pad) const noexcept
        {
            for (const auto& v : voices[(size_t) pad]) if (v.isActive() && v.isLooping() && ! v.isReleasing()) return true;
            return false;
        }
        int    laneLength (int pad) const noexcept;
        double launchBoundary (int pad, int sampleOffset) const noexcept;
        void   launchLane (int pad, int sampleOffset, double anchor, double start, float hitVel) noexcept;
        /** The song-grid pattern start at or before `ppq`: step = playhead position mod LEN. */
        double gridAnchor (int pad, double ppq) const noexcept;
        void   hitSeqPad (int pad, float velocity, int sampleOffset) noexcept;
        void   stopLane (int pad, float fadeMs = 3.0f) noexcept;
        void   addLaneEvents (int pad, int fromOffset) noexcept;
        void   advanceLanes() noexcept;      // block end: segments that have passed
        void applyFx (int group) noexcept;          // param XOR latch | MIDI held | step held
        void stepFx (int group, int fxId, bool on, double endPpq) noexcept;
        void clearStepFx() noexcept;

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
        void recordHit (int pad, float velocity, int sampleOffset, int slice = -1) noexcept;
        void applyPadCommands() noexcept;
        void refreshPadSource (int pad) noexcept;     // own sample or the active bank break
        void switchBank (int slot) noexcept;
        void computeVoiceSettings (const BlockParams&) noexcept;

        EngineBridge& bridge;
        MasterSection master;

        SpscQueue<PadCommand, 128> padCommands;
        std::array<SliceTable, kNumPads> padSlices {};

        std::array<SampleBuffer::Ptr, kNumPads> padSample;           // what each pad plays (audio-thread owned)
        std::array<SampleBuffer::Ptr, kNumPads> ownSample;           // the pad's own sample
        std::array<SliceTable, kNumPads>        ownSlices {};
        std::array<SampleBuffer::Ptr, kBankSlots> bankSample;        // break bank
        // Preset browser audition: plays a buffer from the top, straight to the master (A11).
        SampleBuffer::Ptr previewSample;
        double previewPos = 0.0;
        int    previewFadeIn = 0;
        void   renderPreview (juce::AudioBuffer<float>&, int numSamples) noexcept;
    public:
        bool   isPreviewing() const noexcept { return previewActive.load (std::memory_order_relaxed); }
    private:
        std::atomic<bool> previewActive { false };
        std::array<SliceTable, kBankSlots>        bankSlices {};
        std::array<bool, kNumPads> padFollowBank {};
        int bankActive = 0, bankWanted = 0, bankAtNow = 1;
        std::atomic<int> bankActiveNow { 0 };
        const BlockParams* currentParams = nullptr;
        std::array<PadVoice::Settings, kNumPads> padSettings {};     // this block's settings
        std::array<std::array<PadVoice, kVoicesPerPad>, kNumPads> voices;
        std::array<int, kNumPads>   currentVoice {};
        std::array<float, kNumPads> padPeak {};       // per-block peak for lamps
        std::array<bool, kNumPads>  padFlash {};      // triggered this block
        std::array<std::atomic<int>, kNumPads> padNote {};
        std::array<int,  kNumPads>  padMode {};       // this block's mode per pad
        std::array<int,  kNumPads>  padChoke {};
        std::array<bool, kNumPads>  padStepsWhole {};     // A10
        std::array<float, kNumPads> padVelSens {};
        std::array<bool,  kNumPads> padFixVel {};
        std::array<int,  kNumPads>  padSwap {};       // 0 = off, 1..4
        std::array<float, kNumScenes> sceneGain { 1.0f, 1.0f };
        std::atomic<int> xfadeCut { -1 }, sceneViewNow { 0 };
        std::array<int,  kNumPads>  padGroup {};      // 0..3 = A..D
        std::array<GroupBus, kNumGroups> groups;
        std::array<float, kNumGroups> groupPeak {};   // per-block, for the mixer meters

        // FX state from MIDI: latched (LOCKED notes) and held (MOMENTARY notes).
        std::array<std::atomic<uint32_t>, kNumGroups> fxLatch {};
        std::array<uint32_t, kNumGroups> fxHeld {};
        std::array<uint32_t, kNumGroups> fxStepHeld {};                       // held by sequencer steps
        std::array<std::array<double, fx::kNumFx>, kNumGroups> fxStepEnd {};  // ppq each step-held effect lets go
        // Macros: parameters XOR MIDI latch, | MIDI held.
        uint32_t macroParamMask = 0, macroMidiHeld = 0;
        std::atomic<uint32_t> macroMidiLatch { 0 };
        float macroAmountNow = 0.6f;
        int   macroGroupNow = 4;
        bool  macroLatchNow = false;
        uint32_t macroActiveMask() const noexcept { return (macroParamMask ^ macroMidiLatch.load (std::memory_order_relaxed)) | macroMidiHeld; }
        std::array<std::array<std::array<float, 3>, 16>, kNumGroups> appliedFxParams {};
        std::array<uint32_t, kNumGroups> fxParamMask {};
        std::array<std::array<std::array<float, 3>, 16>, kNumGroups> fxParams {};
        std::atomic<int>      fxTargetGroup { 0 };

        static constexpr int kMaxSeqEvents = 1024;
        std::array<SeqEvent, kMaxSeqEvents> seqEvents {};
        int seqEventCount = 0;
        int seqCursor = 0;          // next event to play; inserts never go before it
        std::array<int64_t, kNumPads> seqSkipOnce {};    // absolute step just live-recorded: don't double it this pass
        std::array<int64_t, kNumPads> seqRollStep {};    // absolute step whose CHANCE was rolled ...
        std::array<bool,    kNumPads> seqRollPlays {};   // ... and whether it plays (ratchets follow it)
        bool    seqWasPlaying = false;
        int64_t seqFirstPass = 0;                        // pattern pass Live started in (FIRST condition)
        std::array<std::array<bool, kVoicesPerPad>, kNumPads> voiceCustom {};   // voice runs its own settings (step tweak / slice seq)
        std::array<SliceRun, kNumPads> sliceRun {};
        std::array<LaneRun, kNumPads>  laneRun {};
        std::array<int, kNumPads> padLaunch {}, padSeqLen {}, padSwapFrom {};
        bool   clockWasPlaying = false, seqFillNow = false;
        std::array<bool, kNumPads> laneQueued {};
        std::array<float, kNumPads> pressNow {};      // A7: smoothed Push 3 pad pressure

        // A8: Mikro MK3 slice hits and note-repeat rolls.
        struct RollRun { bool active = false; int rate = 1; float velocity = 1.0f; int slice = 0; double skipUntil = -1.0e300; };
        std::array<RollRun, kNumPads> rollRun {};
        /** Slice `slice` (1..16) of the pad, or (0) its plain sound: straight to a voice, no SWAP / launch / slice seq. */
        void controllerSliceHit (int pad, int slice, float velocity, int sampleOffset) noexcept;
        void addRollEvents (int numSamples) noexcept;
        const TransportState* lastTransport = nullptr;
        std::array<bool, kNumPads> padSliceSeq {};
        std::array<int,  kNumPads> padSliceSlot {};
        std::array<float, kNumPads> padPitchNow {};
        // A11: layers (round robin / velocity / random), fit to tempo.
        std::array<std::array<SampleBuffer::Ptr, params::kMaxLayers>, kNumPads> layerSample;
        std::array<PadParams, kNumPads> padParamsNow {};
        std::array<int, kNumPads>   layerNext {};
        std::array<float, kNumPads> lastVelocity {};
        std::array<double, kNumPads> padFitFactor {};
        // A12 STRETCH, per pad (worked out with the pad's settings each block).
        std::array<int, kNumPads>    padStretchMode {};    // 0 none, else PadVoice stretch (1..4)
        std::array<double, kNumPads> padStretchRatio {};   // Live's tempo / the break's
        std::array<int, kNumPads>    padGrainSamples {};
        std::array<int, kNumPads>    padGapMode {};
        void applyStretch (int pad, const SampleBuffer& buf, double pitchSemis, PadVoice::Settings& s) const noexcept;
        juce::Random layerRandom { 0x7b5 };
        /** Region, loop and speed of a pad's voice for this buffer (START / END / SLICE, sync, pitch, FIT). */
        void regionAndRate (int pad, const PadParams&, const SampleBuffer&, PadVoice::Settings&) noexcept;
    public:
        /** The tempo a break plays at, from its length (90-180 BPM, see loopBeatsFor). */
        static double tempoOf (const SampleBuffer&) noexcept;
        /** A12: the break's tempo from its slice markers when it has them (onsets on a 1/16 grid), else from its length. */
        static double detectTempo (const SampleBuffer&, const SliceTable& markers) noexcept;

        // ---- A12: MIDI out (what you play, for recording into Live) ------------------
        struct OutEvent { int offset; uint8_t bytes[3]; };
        static constexpr int kMaxOut = 512;
        int getOutCount() const noexcept { return outCount; }
        const OutEvent& getOut (int i) const noexcept { return outEvents[(size_t) i]; }
        static constexpr int kSliceChannel = 2;           // slices go out (and in) on channel 2, notes 36-51
        static constexpr int kSliceBaseNote = 36;
    private:
        std::array<OutEvent, kMaxOut> outEvents {};
        int outCount = 0;
        std::array<std::array<bool, 128>, 2> outHeld {};          // channel 1 / 2: note sounding (sent on, no off yet)
        std::array<std::array<int, 128>, 2> outOffIn {};          // > 0: send the off this many samples from now
        int outBlockLen = 0;
        // Echo guard: what we send out comes back in when the track records the
        // "Twisted Breaks Notes" port (same track): those notes are swallowed once.
        std::array<std::array<uint8_t, 128>, 2> echoPending {}, echoSwallowOff {};
        std::array<std::array<int64_t, 128>, 2> echoDeadline {};
        int64_t engineSamples = 0;
        int     lastBlockLen = 0;
        bool isEcho (int channel, int note, bool on) noexcept;
        void emitNote (int channel, int note, float velocity, int offset, int autoOffSamples = 0) noexcept;   // velocity 0 = off
        void emitPadNote (int pad, float velocity, int offset, int autoOffSamples = 0) noexcept;
        void flushAutoOffs (int numSamples) noexcept;
        bool wasPlayingLast = false;
        void stopAllBreaks() noexcept;     // A12: Live stopped
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

        // RESAMPLE
        juce::AudioBuffer<float> rsBuffer, rsTap;
        std::atomic<int>  rsState { rsIdle }, rsWritten { 0 }, rsLength { 0 };
        std::atomic<int>  rsSource { 0 }, rsBars { 2 };
        std::atomic<bool> rsAtBar { true }, rsCancel { false };
        bool rsTapActive = false;

        // Scope decimation
        int   scopeDecimation = 8;
        int   scopeCounter    = 0;
        std::vector<float> scopeScratch;              // sized in prepare()
    };
}
