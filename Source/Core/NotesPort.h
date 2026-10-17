#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>
#include <mutex>
#include <vector>
#include "Core/EngineBridge.h"

namespace tb
{
    /*
        NotesPort  (A12)
        ---------
        A virtual MIDI output called "Twisted Breaks Notes", one per process,
        carrying what you play on the pads (every instance that has it on).
        Live can record it on any MIDI track (Settings > Link, Tempo & MIDI:
        Track on for its input), which also works for the AU, whose MIDI out
        Live doesn't take. The audio thread only pushes into its instance's
        lock-free queue; a small thread drains the queues every millisecond.
        macOS / Linux (Windows has no virtual MIDI ports).
    */
    class NotesPort final : private juce::Thread
    {
    public:
        static constexpr const char* kName = "Twisted Breaks Notes";
        struct Msg { uint8_t bytes[3]; };
        using Queue = SpscQueue<Msg, 1024>;

        static std::shared_ptr<NotesPort> shared();
        explicit NotesPort (bool createDevice);
        ~NotesPort() override;

        /** Message thread: an instance's queue joins / leaves. */
        void attach (Queue*);
        void detach (Queue*);
        bool isAvailable() const noexcept { return output != nullptr || sink != nullptr; }

        struct Sink { virtual ~Sink() = default; virtual void send (const juce::MidiMessage&) = 0; };
        static void setSinkForTesting (Sink*);
        /** Tests: drain now on the calling thread. */
        void drainForTesting() { drain(); }

    private:
        void run() override;
        void drain();

        std::unique_ptr<juce::MidiOutput> output;
        std::mutex lock;
        std::vector<Queue*> queues;
        static Sink* sink;
        JUCE_DECLARE_NON_COPYABLE (NotesPort)
    };
}
