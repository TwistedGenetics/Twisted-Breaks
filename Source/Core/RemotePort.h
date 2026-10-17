#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>

namespace tb
{
    /*
        RemotePort  (spec addition A9)
        ----------
        A virtual MIDI output called "Twisted Breaks Remote", one per process,
        so controller keys can drive Ableton's own transport: a plugin can't
        press Live's Play / Stop / Record, but Live can MIDI-map them to this
        port (Settings > Link, Tempo & MIDI: Remote on for its input; then
        MIDI Map mode, click Play / Stop / Record, press the key).

            PLAY    note 94 (A#5)   channel 16, 127 pressed / note off released
            STOP    note 93 (A5)
            RECORD  note 95 (B5)

        Created while a controller mode that uses it (Mikro MK3, Push 3) is on.
        macOS / Linux only (Windows has no virtual MIDI ports).
    */
    class RemotePort
    {
    public:
        enum Key { play = 94, stop = 93, record = 95 };
        static constexpr int kChannel = 16;
        static constexpr const char* kName = "Twisted Breaks Remote";

        static std::shared_ptr<RemotePort> shared();
        explicit RemotePort (bool createDevice);

        /** Message thread. */
        void key (Key k, bool down);
        bool isAvailable() const noexcept { return output != nullptr || sink != nullptr; }

        struct Sink { virtual ~Sink() = default; virtual void send (const juce::MidiMessage&) = 0; };
        static void setSinkForTesting (Sink*);       // every port sends here instead (tests)

    private:
        std::unique_ptr<juce::MidiOutput> output;
        static Sink* sink;
        JUCE_DECLARE_NON_COPYABLE (RemotePort)
    };
}
