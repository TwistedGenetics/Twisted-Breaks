#include "Core/NotesPort.h"

namespace tb
{
    NotesPort::Sink* NotesPort::sink = nullptr;

    std::shared_ptr<NotesPort> NotesPort::shared()
    {
        static std::mutex m;
        static std::weak_ptr<NotesPort> w;
        std::lock_guard<std::mutex> g (m);
        if (auto s = w.lock()) return s;
        auto s = std::make_shared<NotesPort> (sink == nullptr);
        w = s;
        return s;
    }

    NotesPort::NotesPort (bool createDevice) : juce::Thread ("Twisted Breaks Notes")
    {
       #if JUCE_MAC || JUCE_LINUX
        if (createDevice) output = juce::MidiOutput::createNewDevice (kName);
       #else
        juce::ignoreUnused (createDevice);
       #endif
        if (createDevice) startThread (juce::Thread::Priority::high);
    }

    NotesPort::~NotesPort() { stopThread (2000); }

    void NotesPort::attach (Queue* q)
    {
        std::lock_guard<std::mutex> g (lock);
        if (std::find (queues.begin(), queues.end(), q) == queues.end()) queues.push_back (q);
    }

    void NotesPort::detach (Queue* q)
    {
        std::lock_guard<std::mutex> g (lock);
        queues.erase (std::remove (queues.begin(), queues.end(), q), queues.end());
    }

    void NotesPort::drain()
    {
        std::lock_guard<std::mutex> g (lock);
        for (auto* q : queues)
        {
            Msg m;
            while (q->pop (m))
            {
                const juce::MidiMessage mm (m.bytes[0], m.bytes[1], m.bytes[2]);
                if (sink != nullptr)        sink->send (mm);
                else if (output != nullptr) output->sendMessageNow (mm);
            }
        }
    }

    void NotesPort::run()
    {
        while (! threadShouldExit())
        {
            drain();
            wait (1);
        }
    }

    void NotesPort::setSinkForTesting (Sink* s) { sink = s; }
}
