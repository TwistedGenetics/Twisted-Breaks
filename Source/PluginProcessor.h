#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/EngineBridge.h"
#include "Core/Parameters.h"
#include "Core/TransportSync.h"
#include "Engine/AudioEngine.h"
#include "Core/SampleManager.h"
#include "Core/PadLights.h"
#include "Core/SnapshotManager.h"
#include "Core/Mutator.h"
#include "Core/Resampler.h"
#include "Core/BreakBank.h"
#include "Core/PushController.h"
#include "Core/MikroTester.h"

/*
    TwistedBreaksProcessor
    ----------------------
    The host-facing shell. Keeps itself thin on purpose:
      * owns the parameter tree (APVTS), the TransportSync and the AudioEngine
      * processBlock() reads the host playhead, snapshots parameters and hands
        everything to AudioEngine::process()
      * owns the SampleManager (sample loading + hand-off to the engine)
      * get/setStateInformation() serialise parameters + non-parameter state
        (from Phase 8, StateManager takes over the non-parameter part)
*/
class TwistedBreaksProcessor final : public juce::AudioProcessor,
                                     private juce::AudioProcessorParameter::Listener,
                                     private juce::AsyncUpdater
{
public:
    TwistedBreaksProcessor();
    ~TwistedBreaksProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool   acceptsMidi() const override  { return true; }
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int  getNumPrograms() override    { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Accessors for the editor (message thread)
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return apvts; }
    tb::EngineBridge& getBridge() noexcept { return bridge; }
    tb::AudioEngine&  getEngine() noexcept { return engine; }
    tb::SampleManager& getSamples() noexcept { return samples; }
    tb::PadLights&     getLights() noexcept  { return lights; }
    tb::SnapshotManager& getSnapshots() noexcept { return *snapshots; }
    tb::BreakBank&       getBank() noexcept      { return *bank; }
    tb::Mutator&         getMutator() noexcept   { return *mutator; }
    tb::Resampler&       getResampler() noexcept { return *resampler; }
    tb::PushController&  getPush() noexcept      { return *push; }
    tb::MikroTester&     getMikro() noexcept     { return *mikro; }

    // ---- MUTATE (see Mutator) -----------------------------------------------------
    /** Message thread: arm a pattern change; returns a frame it replaced (not applied). */
    tb::MutationFrame* armMutationFrame (tb::MutationFrame* f) noexcept { return armedMutation.exchange (f); }
    tb::MutationFrame* takeAppliedMutationFrame() noexcept { return appliedMutation.exchange (nullptr); }
    bool hasArmedMutation() const noexcept { return armedMutation.load() != nullptr; }
    bool isMutationFrameInUse (const tb::MutationFrame* f) const noexcept { return f == armedMutation.load() || f == appliedMutation.load(); }
    /** Message thread, audio not running: apply the armed frame here. */
    void applyMutationNow() noexcept { if (auto* f = armedMutation.load()) applyMutationFrame (f); }

    // ---- Snapshot switching (see SnapshotManager) ----------------------------------
    /** Message thread: arm a frame for the audio thread; returns a frame it replaced (not applied). */
    tb::SnapshotFrame* armSnapshotFrame (tb::SnapshotFrame*) noexcept;
    tb::SnapshotFrame* takeAppliedSnapshotFrame() noexcept { return appliedFrame.exchange (nullptr); }
    void releaseSnapshotOverride (tb::SnapshotFrame* f) noexcept { overrideFrame.compare_exchange_strong (f, nullptr); }
    bool isSnapshotFrameInUse (const tb::SnapshotFrame* f) const noexcept
    {
        return f == armedFrame.load() || f == overrideFrame.load() || f == appliedFrame.load();
    }
    uint64_t getBlockEpoch() const noexcept { return blockEpoch.load(); }
    /** Fills the engine's parameter view from any source of raw parameter values. */
    void fillParamsFrom (tb::AudioEngine::BlockParams&, const std::function<float (const std::atomic<float>*)>&) const;

    /** UI pad press: queued lock-free to the audio thread. */
    void triggerPadFromUi (int pad, float velocity, bool noteOn);
    void requestPanic() noexcept { bridge.panicRequested.store (true); }

    /**
        Tells the host "my saved state is out of date, fetch it again".
        Ableton keeps its own cached copy of an Audio Unit's state and uses it
        for saving the set, duplicating the track and undo; it only refreshes
        that copy when the plugin says so. Called (debounced) after every
        knob / switch gesture in the plugin window and after any sample load,
        clear or MIDI-note change. Never from the audio thread.
    */
    void markStateChanged();

    // ---- Performance FX keys (state, not parameters) ----------------------------
    /** LOCKED (click / note toggles) or MOMENTARY (on while held), per effect. */
    void setFxLocked (int fxId, bool locked);
    bool isFxLocked (int fxId) const noexcept { return ((engine.getFxLockedMask() >> fxId) & 1u) != 0; }
    /** Group the FX grid and the FX MIDI notes act on (0..3). */
    void setFxGroup (int g);
    /** Message thread: switch every group's effects off (PANIC button). */
    void allFxOff();

    // ---- Slice sequencer edits (message thread) -----------------------------------
    enum class SliceOp { shuffle, random, reverseOrder, half, rotateLeft, rotateRight, reset };
    /** The slot the pad's SLOT parameter points at (0..7). */
    int  sliceSlotOf (int pad) const;
    /** Turns an unwritten slot (playing the break as recorded) into editable steps. */
    void materializeSliceSlot (int pad, int slot);
    /** One-press variations on the pad's current slot. */
    void sliceSeqOp (int pad, SliceOp op, juce::Random& rnd);

    /**
        SPREAD: puts each slice of `pad` on its own pad (pad, pad+1, ...),
        sharing the same decoded sample, each in SLICE mode playing slice
        1, 2, 3 ... Level, pitch, reverse, velocity and choke are copied from
        the source pad. Returns how many pads were filled.
    */
    int spreadSlices (int pad);

    static constexpr int kStateVersion = 1;

private:
    tb::EngineBridge                    bridge;
    juce::AudioProcessorValueTreeState  apvts;
    tb::TransportSync                   transport;
    tb::AudioEngine                     engine { bridge };
    tb::SampleManager                   samples { engine };   // after engine: destroyed first
    tb::PadLights                       lights { bridge, engine };   // controller LEDs (iRig Pads)
    std::unique_ptr<tb::BreakBank>       bank;        // after samples: destroyed first
    std::unique_ptr<tb::SnapshotManager> snapshots;
    std::unique_ptr<tb::Mutator>         mutator;
    std::unique_ptr<tb::Resampler>       resampler;
    std::unique_ptr<tb::PushController>  push;
    std::unique_ptr<tb::MikroTester>     mikro;      // Mikro MK3 USB test (not saved)       // A7: Push 3 mode (after mutator / resampler: destroyed first)

    template <typename Load> void fillParams (tb::AudioEngine::BlockParams&, Load&&) const;
    void applySnapshotFrame (tb::SnapshotFrame*) noexcept;
    static int switchOffset (const tb::TransportState&, int quant, int numSamples, int bars = 1) noexcept;
    void applyMutationFrame (tb::MutationFrame*) noexcept;
    std::atomic<tb::MutationFrame*> armedMutation { nullptr }, appliedMutation { nullptr };

    std::atomic<tb::SnapshotFrame*> armedFrame { nullptr }, overrideFrame { nullptr }, appliedFrame { nullptr };
    std::atomic<uint64_t> blockEpoch { 0 };
    juce::MidiBuffer midiFirst, midiSecond;

    // Cached raw parameter pointers: atomic loads only in processBlock().
    std::atomic<float>* masterGainParam    = nullptr;
    std::atomic<float>* masterLimiterParam = nullptr;
    std::atomic<float>* masterPanicParam   = nullptr;
    std::atomic<float>* seqRunParam = nullptr;
    std::atomic<float>* seqLengthParam = nullptr;
    std::atomic<float>* seqResParam = nullptr;
    std::atomic<float>* seqQuantParam = nullptr;
    std::atomic<float>* seqFillParam = nullptr;
    std::array<std::atomic<float>*, 8> macroOnParam {};
    std::atomic<float>* macroAmountParam = nullptr;
    std::atomic<float>* xfadeParam = nullptr;
    std::atomic<float>* xfadeCurveParam = nullptr;
    std::atomic<float>* sceneViewParam = nullptr;
    std::atomic<float>* macroGroupParam  = nullptr;
    std::atomic<float>* macroLatchParam  = nullptr;
    std::atomic<float>* bankActiveParam = nullptr;
    std::atomic<float>* bankAtParam = nullptr;

    struct PadParamPointers
    {
        std::atomic<float>* gain = nullptr;
        std::atomic<float>* pan = nullptr;
        std::atomic<float>* pitch = nullptr;
        std::atomic<float>* start = nullptr;
        std::atomic<float>* end = nullptr;
        std::atomic<float>* reverse = nullptr;
        std::atomic<float>* mode = nullptr;
        std::atomic<float>* sync = nullptr;
        std::atomic<float>* rate = nullptr;
        std::atomic<float>* choke = nullptr;
        std::atomic<float>* velSens = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* solo = nullptr;
        std::atomic<float>* slice = nullptr;
        std::atomic<float>* swap = nullptr;
        std::atomic<float>* swapAt = nullptr;
        std::atomic<float>* group = nullptr;
        std::atomic<float>* fixVel = nullptr;
        std::atomic<float>* sliceSeq = nullptr;
        std::atomic<float>* sliceSlot = nullptr;
        std::atomic<float>* followBank = nullptr;
        std::atomic<float>* character = nullptr;
        std::atomic<float>* launch = nullptr;
        std::atomic<float>* seqLen = nullptr;
        std::atomic<float>* swapFrom = nullptr;
        std::atomic<float>* press = nullptr;
    };
    std::array<PadParamPointers, tb::kNumPads> padParams {};

    struct GroupParamPointers
    {
        std::atomic<float>* level = nullptr;
        std::atomic<float>* pan = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* solo = nullptr;
        std::atomic<float>* bypass = nullptr;
        std::array<std::atomic<float>*, 16> fxOn {};
        std::array<std::array<std::atomic<float>*, 3>, 16> fxParam {};
    };
    std::array<GroupParamPointers, tb::kNumGroups> groupParams {};

    // Parameter listener: a finished UI gesture means the saved state changed.
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool gestureIsStarting) override;
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TwistedBreaksProcessor)
};
