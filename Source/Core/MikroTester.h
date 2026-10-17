#pragma once

#include <juce_events/juce_events.h>
#include <array>
#include <deque>
#include <functional>
#include <vector>
#include "Core/EngineBridge.h"
#include "Core/MikroProtocol.h"
#include "Core/MikroUsb.h"

namespace tb
{
    /*
        MikroTester  (step 1 of Mikro MK3 support)
        -----------
        CONTROLLERS > "Mikro MK3 test": talks to the Mikro over USB directly and
        proves each part works on the real hardware before the full mode is
        built on it:

          * opens the device (and says exactly why not, if it can't)
          * pads light in 16 colours, buttons dim, the touch strip lit
          * the screen reads TWISTED BREAKS / MIKRO MK3 TEST
          * every pad, button, the encoder and the strip show what they send,
            on the Mikro's screen, its lights and the plugin's status strip
          * "Copy Mikro test report" puts a text log on the clipboard

        USB input arrives on the device thread and is queued (lock-free) to
        the message thread, which does everything else at 30 Hz.
    */
    class MikroTester final : private juce::Timer
    {
    public:
        MikroTester();
        ~MikroTester() override;

        void start();
        void stop();
        bool isRunning() const noexcept { return running; }
        bool isConnected() const noexcept { return connected; }

        juce::String getStatus() const { return status; }
        juce::String getLastEvent() const { return lastEvent; }
        /** Everything worth sending back: status, sizes, counts, raw reports, the event log, write errors. */
        juce::String getReport() const;

        // ---- Testing (no USB) -------------------------------------------------------------
        /** Writes go here instead of the device; the tester counts as connected. */
        void startForTesting (std::function<bool (const std::vector<uint8_t>&)> writer);
        /** As if the device sent this report (any thread). */
        void feedReport (const uint8_t* data, int size);
        void process (double nowMs);          // the timer does this
        const mikro::Lights& getLights() const noexcept { return lights; }
        const mikro::Screen& getScreen() const noexcept { return screen; }

    private:
        void timerCallback() override;
        bool send (const std::vector<uint8_t>&);
        void sendLights();
        void sendScreen();
        void showIntro();
        void handle (const mikro::Event&, double nowMs);
        void log (const juce::String&);
        static int padColourFor (int pad) noexcept { return 1 + pad % 16; }

        struct Raw { uint8_t size = 0; uint8_t data[64] {}; };
        SpscQueue<Raw, 512> inbox;
        std::atomic<bool> removedFlag { false };
        std::atomic<uint32_t> reportsIn { 0 };

        MikroUsb usb;
        std::function<bool (const std::vector<uint8_t>&)> testWriter;
        bool running = false, connected = false;
        mikro::InputParser parser;
        mikro::Lights lights;
        mikro::Screen screen;
        bool lightsDirty = false, screenDirty = false;
        double lastScreenMs = -1.0e9;
        int lightsLength = 81;                     // 0x80 + 80; 91 if the device wants that
        int encoderTotal = 0, retryCountdown = 0;
        juce::String status, lastEvent, line1, line2;

        // Report log.
        std::deque<juce::String> events;
        std::array<juce::String, 4> firstRaw;      // first report of id 0x01 / 0x02 / other, as hex
        std::array<uint32_t, 3> counts {};          // button reports, pad reports, other
        juce::StringArray writeErrors;
        uint32_t writesOk = 0;
        juce::String deviceText;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MikroTester)
    };
}
