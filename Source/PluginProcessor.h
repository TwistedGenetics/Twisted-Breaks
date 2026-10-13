#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/EngineBridge.h"
#include "Core/Parameters.h"
#include "Core/TransportSync.h"
#include "Engine/AudioEngine.h"
#include "Core/SampleManager.h"

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
class TwistedBreaksProcessor final : public juce::AudioProcessor
{
public:
    TwistedBreaksProcessor();
    ~TwistedBreaksProcessor() override = default;

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

    /** UI pad press: queued lock-free to the audio thread. */
    void triggerPadFromUi (int pad, float velocity, bool noteOn);
    void requestPanic() noexcept { bridge.panicRequested.store (true); }

    static constexpr int kStateVersion = 1;

private:
    tb::EngineBridge                    bridge;
    juce::AudioProcessorValueTreeState  apvts;
    tb::TransportSync                   transport;
    tb::AudioEngine                     engine { bridge };
    tb::SampleManager                   samples { engine };   // after engine: destroyed first

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
    };
    std::array<PadParamPointers, tb::kNumPads> padParams {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TwistedBreaksProcessor)
};
