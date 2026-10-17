#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <memory>

class TwistedBreaksProcessor;

namespace tb
{
    /*
        Exporter (A11 part 5)
        ---------------------
        WAV: renders the selected pad (soloed) or the whole kit for N bars at
        Live's tempo, offline, in a second copy of the plugin made from this
        one's state (controller settings left out, so nothing grabs Push or the
        Mikro). The copy loads its samples, then a worker thread runs its
        processBlock from bar 1 with a fake playing transport and writes a
        24-bit WAV into ~/Music/Twisted Breaks/Exports. Live keeps playing
        undisturbed.

        MIDI: the main sequencer as a MIDI clip (each pad on its own MIDI note,
        velocity, micro-timing and ratchets), N bars at Live's tempo.

        Message thread, except the render itself.
    */
    class Exporter final : private juce::Timer
    {
    public:
        explicit Exporter (TwistedBreaksProcessor&);
        ~Exporter() override;

        static juce::File defaultFolder();
        void setFolderForTesting (const juce::File& f) { folder = f; }
        juce::File getFolder() const { return folder; }

        /** pad >= 0: that pad alone; -1: the whole kit. `done` gets the file (or an error). */
        bool startWav (int pad, int bars, std::function<void (juce::Result, juce::File)> done);
        bool isBusy() const noexcept { return busy; }
        float getProgress() const noexcept { return progress.load(); }

        /** pad >= 0: that pad's lane only; -1: every lane. */
        juce::Result writeMidi (int pad, int bars, const juce::File&);
        juce::File   midiFileFor (int pad, int bars) const;
        juce::File   wavFileFor (int pad, int bars) const;

        juce::File getLastFile() const { return lastFile; }

        /** A12 RENDER AKAI: the pad's sample through the Akai cyclic stretch (time factor 25-2000 %,
            pitch +-36 st, cycle, Classic / Revised), written next to the Exports as a new WAV. */
        static juce::AudioBuffer<float> akaiRender (const juce::AudioBuffer<float>& src, double rate, double timeFactor,
                                                    float semis, float cycleMs, bool revised);
        juce::Result renderAkai (int pad, double timeFactor, float semis, float cycleMs, bool revised, juce::File& out);

    private:
        void timerCallback() override;
        void finish (juce::Result, juce::File);

        TwistedBreaksProcessor& processor;
        juce::File folder;
        juce::File lastFile;

        struct Job;
        std::unique_ptr<Job> job;
        bool busy = false;
        std::atomic<float> progress { 0.0f };

        JUCE_DECLARE_WEAK_REFERENCEABLE (Exporter)
    };
}
