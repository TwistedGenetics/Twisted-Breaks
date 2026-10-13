#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/EngineBridge.h"
#include "Core/Parameters.h"
#include "Core/TransportSync.h"
#include "Engine/AudioEngine.h"
#include "Core/SampleManager.h"
#include "Core/PadLights.h"

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

    // Cached raw parameter pointers: atomic loads only in processBlock().
    std::atomic<float>* masterGainParam    = nullptr;
    std::atomic<float>* masterLimiterParam = nullptr;
    std::atomic<float>* masterPanicParam   = nullptr;

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
    };
    std::array<PadParamPointers, tb::kNumPads> padParams {};

    struct GroupParamPointers
    {
        std::atomic<float>* level = nullptr;
        std::atomic<float>* pan = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* solo = nullptr;
        std::atomic<float>* bypass = nullptr;
    };
    std::array<GroupParamPointers, tb::kNumGroups> groupParams {};

    // Parameter listener: a finished UI gesture means the saved state changed.
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool gestureIsStarting) override;
    void handleAsyncUpdate() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TwistedBreaksProcessor)
};
