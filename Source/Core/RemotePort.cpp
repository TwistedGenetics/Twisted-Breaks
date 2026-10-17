#include "Core/RemotePort.h"
#include <mutex>

namespace tb
{
    RemotePort::Sink* RemotePort::sink = nullptr;

    std::shared_ptr<RemotePort> RemotePort::shared()
    {
        static std::mutex m;
        static std::weak_ptr<RemotePort> w;
        std::lock_guard<std::mutex> g (m);
        if (auto s = w.lock()) return s;
        auto s = std::make_shared<RemotePort> (sink == nullptr);
        w = s;
        return s;
    }

    RemotePort::RemotePort (bool createDevice)
    {
       #if JUCE_MAC || JUCE_LINUX
        if (createDevice) output = juce::MidiOutput::createNewDevice (kName);
       #else
        juce::ignoreUnused (createDevice);
       #endif
    }

    void RemotePort::key (Key k, bool down)
    {
        const auto m = down ? juce::MidiMessage::noteOn (kChannel, (int) k, (juce::uint8) 127)
                            : juce::MidiMessage::noteOff (kChannel, (int) k);
        if (sink != nullptr)        sink->send (m);
        else if (output != nullptr) output->sendMessageNow (m);
    }

    void RemotePort::setSinkForTesting (Sink* s) { sink = s; }
}
