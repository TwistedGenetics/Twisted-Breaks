#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>
#include <vector>

namespace tb
{
    /*
        MikroUsb
        --------
        Raw USB HID access to a Maschine Mikro MK3 (no NI software). macOS
        only (IOKit); elsewhere open() reports that it isn't supported.

        Input reports arrive on the device's own thread (a CFRunLoop) through
        onReport; write() sends an output report (first byte = report id)
        from the calling thread. open() says plainly why it failed, with the
        IOKit code, so a test run on the real hardware tells us what to fix:
        "held by another app" (NI's background service), "not permitted",
        "not found".
    */
    class MikroUsb
    {
    public:
        MikroUsb();
        ~MikroUsb();

        static bool isSupported() noexcept;

        bool open();
        void close();
        bool isOpen() const noexcept;

        /** Sends one output report. False (and getLastError()) if the device refused it. */
        bool write (const std::vector<uint8_t>& report);

        juce::String getLastError() const;
        juce::String describe() const;      // product name, report sizes

        /** The device thread: one input report (data[0] = report id). */
        std::function<void (const uint8_t*, int)> onReport;
        /** The device thread: unplugged. */
        std::function<void()> onRemoved;

        struct Impl;
    private:
        std::unique_ptr<Impl> impl;
        JUCE_DECLARE_NON_COPYABLE (MikroUsb)
    };
}
