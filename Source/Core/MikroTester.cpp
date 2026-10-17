#include "Core/MikroTester.h"

namespace tb
{
    using namespace mikro;

    MikroTester::MikroTester()
    {
        usb.onReport = [this] (const uint8_t* d, int n) { feedReport (d, n); };
        usb.onRemoved = [this] { removedFlag = true; };
    }

    MikroTester::~MikroTester()
    {
        stop();
    }

    void MikroTester::log (const juce::String& s)
    {
        events.push_back (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ") + s);
        while (events.size() > 60) events.pop_front();
    }

    void MikroTester::feedReport (const uint8_t* data, int size)
    {
        Raw r;
        r.size = (uint8_t) juce::jlimit (0, 64, size);
        std::memcpy (r.data, data, (size_t) r.size);
        inbox.push (r);
        reportsIn.fetch_add (1);
    }

    bool MikroTester::send (const std::vector<uint8_t>& rep)
    {
        const bool ok = testWriter ? testWriter (rep) : usb.write (rep);
        if (ok) ++writesOk;
        else
        {
            const auto e = testWriter ? juce::String ("refused") : usb.getLastError();
            if (writeErrors.size() < 20) writeErrors.add (e);
        }
        return ok;
    }

    void MikroTester::sendLights()
    {
        auto rep = lights.report();
        rep.resize ((size_t) lightsLength, 0);
        if (! send (rep) && lightsLength == 81)
        {
            // Some descriptions of the device give the lights report as 90 bytes: try that once.
            lightsLength = 91;
            rep.resize (91, 0);
            if (send (rep)) log ("lights: the device wants a 91-byte report");
            else            lightsLength = 81;
        }
        lightsDirty = false;
    }

    void MikroTester::sendScreen()
    {
        screen.clear();
        screen.text (0, 0, line1.toRawUTF8(), 2);
        screen.text (0, 20, line2.toRawUTF8(), 1);
        for (auto& r : screen.reports()) send (r);
        screenDirty = false;
    }

    void MikroTester::showIntro()
    {
        lights.clear();
        for (int b = 0; b < kNumLitButtons; ++b) lights.setButton (b, levelDim);
        for (int p = 0; p < mikro::kNumPads; ++p) lights.setPad (p, padColourFor (p), 2);
        for (int s = 0; s < kNumStripLeds; ++s) lights.setStrip (s, levelDim);
        line1 = "TWISTED";
        line2 = "BREAKS - MIKRO MK3 TEST";
        lightsDirty = screenDirty = true;
    }

    void MikroTester::start()
    {
        if (running) return;
        testWriter = nullptr;
        events.clear(); writeErrors.clear(); counts.fill (0); writesOk = 0; firstRaw.fill ({}); encoderTotal = 0;
        running = true;
        connected = usb.open();
        if (connected)
        {
            deviceText = usb.describe();
            status = "Connected: " + deviceText;
            log ("opened: " + deviceText);
            showIntro();
            process (juce::Time::getMillisecondCounterHiRes());
        }
        else
        {
            status = usb.getLastError();
            log ("open failed: " + status);
        }
        startTimerHz (30);
    }

    void MikroTester::startForTesting (std::function<bool (const std::vector<uint8_t>&)> writer)
    {
        testWriter = std::move (writer);
        running = connected = true;
        status = "Connected (test)";
        showIntro();
    }

    void MikroTester::stop()
    {
        if (! running) return;
        stopTimer();
        if (connected)
        {
            lights.clear();
            sendLights();
            line1 = {}; line2 = {};
            sendScreen();
        }
        if (! testWriter) usb.close();
        running = connected = false;
        status = "Stopped";
    }

    void MikroTester::timerCallback()
    {
        // Not found / couldn't open: try again every 2 s (plugged in later, NI software quit).
        if (! connected)
        {
            if (++retryCountdown >= 60)
            {
                retryCountdown = 0;
                connected = usb.open();
                if (connected)
                {
                    deviceText = usb.describe();
                    status = "Connected: " + deviceText;
                    log ("opened: " + deviceText);
                    showIntro();
                }
                else status = usb.getLastError();
            }
            return;
        }
        process (juce::Time::getMillisecondCounterHiRes());
    }

    void MikroTester::process (double nowMs)
    {
        if (! running || ! connected) return;
        if (removedFlag.exchange (false))
        {
            connected = false;
            status = "Unplugged - plug it back in";
            log ("removed");
            if (! testWriter) usb.close();
            return;
        }

        Raw r;
        std::vector<Event> evs;
        while (inbox.pop (r))
        {
            const int id = r.size > 0 ? r.data[0] : -1;
            const int slot = id == 0x01 ? 0 : id == 0x02 ? 1 : 2;
            ++counts[(size_t) slot];
            if (firstRaw[(size_t) slot].isEmpty())
                firstRaw[(size_t) slot] = juce::String::toHexString (r.data, r.size, 1);
            evs.clear();
            parser.parse (r.data, r.size, evs);
            for (auto& e : evs) handle (e, nowMs);
        }

        if (lightsDirty) sendLights();
        if (screenDirty && nowMs - lastScreenMs > 80.0)
        {
            lastScreenMs = nowMs;
            sendScreen();
        }
    }

    void MikroTester::handle (const Event& e, double)
    {
        juce::String text;
        switch (e.kind)
        {
            case Event::pad:
            {
                const int padNo = (3 - e.index / 4) * 4 + e.index % 4 + 1;   // Maschine's pad number (1 = bottom left; index 0 = top left)
                if (e.padEvent == 0x40)
                {
                    line2 = "PRESSURE " + juce::String (e.value);
                    screenDirty = true;
                    lastEvent = "PAD " + juce::String (padNo) + " pressure " + juce::String (e.value);
                    return;                                   // not logged: too many
                }
                lights.setPad (e.index, e.down ? white : padColourFor (e.index), e.down ? 3 : 2);
                lightsDirty = true;
                text = "PAD " + juce::String (padNo) + (e.down ? " DOWN" : " UP") + "  value " + juce::String (e.value)
                       + "  (event 0x" + juce::String::toHexString (e.padEvent) + ")";
                line1 = "PAD " + juce::String (padNo);
                line2 = (e.down ? "HIT " : "UP ") + juce::String (e.value);
                break;
            }
            case Event::button:
                if (e.index < kNumLitButtons) { lights.setButton (e.index, e.down ? levelBright : levelDim); lightsDirty = true; }
                text = juce::String (buttonName (e.index)) + (e.down ? " DOWN" : " UP");
                if (e.down) { line1 = buttonName (e.index); line2 = "BUTTON " + juce::String (e.index); }
                break;
            case Event::encoder:
                encoderTotal += e.value;
                text = "ENCODER " + juce::String (e.value > 0 ? "+" : "") + juce::String (e.value) + "  total " + juce::String (encoderTotal);
                line1 = "ENCODER";
                line2 = juce::String (encoderTotal);
                break;
            case Event::strip:
            {
                text = e.value == 0 ? juce::String ("STRIP let go") : "STRIP " + juce::String (e.value);
                const int led = e.value > 0 ? juce::jlimit (0, kNumStripLeds - 1, (e.value - 1) * kNumStripLeds / 200) : -1;
                for (int s = 0; s < kNumStripLeds; ++s) lights.setStrip (s, s == led ? levelBright : levelDim);
                lightsDirty = true;
                if (e.value > 0) { line1 = "STRIP"; line2 = juce::String (e.value); }
                break;
            }
        }
        screenDirty = true;
        lastEvent = text;
        log (text);
    }

    juce::String MikroTester::getReport() const
    {
        juce::String s;
        s << "TWISTED BREAKS - MIKRO MK3 TEST REPORT\n";
        s << "status: " << status << "\n";
        s << "device: " << deviceText << "\n";
        s << "reports in: " << (int) counts[0] << " button, " << (int) counts[1] << " pad, " << (int) counts[2] << " other\n";
        s << "first button report: " << firstRaw[0] << "\n";
        s << "first pad report: " << firstRaw[1] << "\n";
        s << "first other report: " << firstRaw[2] << "\n";
        s << "lights report: " << lightsLength << " bytes; writes ok: " << (int) writesOk << "\n";
        if (! writeErrors.isEmpty()) s << "write errors:\n  " << writeErrors.joinIntoString ("\n  ") << "\n";
        s << "events:\n";
        for (auto& e : events) s << "  " << e << "\n";
        return s;
    }
}
