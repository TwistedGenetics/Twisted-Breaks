#include "Core/PadLights.h"
#include "Engine/AudioEngine.h"

namespace tb
{
    const juce::Identifier PadLights::lightsId { "LIGHTS" };

    namespace
    {
        // Hardware pad -> first CC of its pair (iRig Pads manual, section 4).
        constexpr int kFirstCc[kNumPads] = { 94, 96, 98, 100, 86, 88, 90, 92, 78, 80, 82, 84, 70, 72, 74, 76 };

        // Notes each hardware pad sends on the default scene (General MIDI kit, C1 = 36):
        // B0 C1 D1 E1 / C#1 D#1 F#1 G#1 / D2 A1 F1 A#1 / D#2 F2 C#2 E2
        constexpr int kDefaultNotes[kNumPads] = { 35, 36, 38, 40, 37, 39, 42, 44, 50, 45, 41, 46, 51, 53, 49, 52 };

        constexpr int kChannel = 1;
        constexpr double kFrameMs = 1000.0 / 30.0;
    }

    PadLights::PadLights (EngineBridge& b, AudioEngine& e) : bridge (b), engine (e)
    {
        learned.fill (-1);
        sentRed.fill (-1);
        sentGreen.fill (-1);
    }

    PadLights::~PadLights()
    {
        stopTimer();
        cancelPendingUpdate();
        const juce::ScopedLock sl (lock);
        sendAllOff();
    }

    //==========================================================================
    int PadLights::defaultSceneNote (int hw) noexcept
    {
        return juce::isPositiveAndBelow (hw, kNumPads) ? kDefaultNotes[hw] : -1;
    }

    int PadLights::ccFor (int hw, bool red, bool swap) noexcept
    {
        if (! juce::isPositiveAndBelow (hw, kNumPads)) return -1;
        const bool first = red != swap;     // manual order: RED-GREEN
        return kFirstCc[hw] + (first ? 0 : 1);
    }

    int PadLights::hardwarePadFor (int pluginPad) const
    {
        if (! juce::isPositiveAndBelow (pluginPad, kNumPads)) return -1;
        const juce::ScopedLock sl (lock);
        if (learned[(size_t) pluginPad] >= 0) return learned[(size_t) pluginPad];

        const int note = engine.getPadNote (pluginPad);
        for (int hw = 0; hw < kNumPads; ++hw)
            if (kDefaultNotes[hw] == note) return hw;
        return -1;
    }

    juce::StringArray PadLights::availableDevices()
    {
        juce::StringArray names, others;
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            (d.name.containsIgnoreCase ("iRig Pads") ? names : others).addIfNotAlreadyThere (d.name);
        names.addArray (others);
        return names;
    }

    //==========================================================================
    void PadLights::setDevice (const juce::String& name)
    {
        {
            const juce::ScopedLock sl (lock);
            if (name == deviceName) return;
            sendAllOff();
            output.reset();
            deviceName = name;
            learning = false;
            lastOpenAttemptMs = -1.0e9;
        }
        updateTimer();
        if (onSettingsChanged) onSettingsChanged();
    }

    juce::String PadLights::getDevice() const     { const juce::ScopedLock sl (lock); return deviceName; }
    bool PadLights::isConnected() const           { const juce::ScopedLock sl (lock); return output != nullptr || testSink != nullptr; }
    bool PadLights::getSwapColours() const        { const juce::ScopedLock sl (lock); return swapColours; }
    bool PadLights::isLearning() const            { const juce::ScopedLock sl (lock); return learning; }
    int  PadLights::getLearnStep() const          { const juce::ScopedLock sl (lock); return learnStep; }

    void PadLights::setSwapColours (bool s)
    {
        {
            const juce::ScopedLock sl (lock);
            if (s == swapColours) return;
            sendAllOff();
            swapColours = s;
        }
        if (onSettingsChanged) onSettingsChanged();
    }

    void PadLights::startLearn()
    {
        const juce::ScopedLock sl (lock);
        learning = true;
        learnStep = 0;
        learnMidiCount = bridge.midiEventCount.load();
        learned.fill (-1);
        testStartMs = -1.0;
    }

    void PadLights::stopLearn()
    {
        const juce::ScopedLock sl (lock);
        learning = false;
    }

    bool PadLights::hasLearnedLayout() const
    {
        const juce::ScopedLock sl (lock);
        for (int v : learned) if (v >= 0) return true;
        return false;
    }

    void PadLights::forgetLearnedLayout()
    {
        {
            const juce::ScopedLock sl (lock);
            learned.fill (-1);
            sendAllOff();
        }
        if (onSettingsChanged) onSettingsChanged();
    }

    void PadLights::runTest()
    {
        const juce::ScopedLock sl (lock);
        learning = false;
        testStartMs = juce::Time::getMillisecondCounterHiRes();
    }

    void PadLights::setSinkForTesting (Sink* s)
    {
        const juce::ScopedLock sl (lock);
        testSink = s;
        sentRed.fill (-1);
        sentGreen.fill (-1);
    }

    void PadLights::updateTimer()
    {
        const bool want = getDevice().isNotEmpty();
        if (want && ! isTimerRunning())  startTimer ((int) kFrameMs);
        if (! want && isTimerRunning())  stopTimer();
    }

    //==========================================================================
    juce::ValueTree PadLights::toValueTree() const
    {
        const juce::ScopedLock sl (lock);
        juce::ValueTree t (lightsId);
        t.setProperty ("device", deviceName, nullptr);
        t.setProperty ("swapColours", swapColours, nullptr);
        juce::StringArray map;
        for (int v : learned) map.add (juce::String (v));
        t.setProperty ("layout", map.joinIntoString (","), nullptr);
        return t;
    }

    void PadLights::fromValueTree (const juce::ValueTree& t)
    {
        {
            const juce::ScopedLock sl (lock);
            swapColours = (bool) t.getProperty ("swapColours", false);
            learned.fill (-1);
            const auto map = juce::StringArray::fromTokens (t.getProperty ("layout", "").toString(), ",", "");
            for (int i = 0; i < juce::jmin (kNumPads, map.size()); ++i)
                learned[(size_t) i] = juce::jlimit (-1, kNumPads - 1, map[i].getIntValue());
        }
        // Not through setDevice(): restoring a set isn't a user edit.
        const auto name = t.getProperty ("device", "").toString();
        {
            const juce::ScopedLock sl (lock);
            if (name != deviceName) { sendAllOff(); output.reset(); deviceName = name; lastOpenAttemptMs = -1.0e9; }
        }
        updateTimer();
    }

    //==========================================================================
    void PadLights::hiResTimerCallback()
    {
        tick (juce::Time::getMillisecondCounterHiRes());
    }

    void PadLights::handleAsyncUpdate()
    {
        if (onSettingsChanged) onSettingsChanged();
    }

    void PadLights::tryOpen (double nowMs)
    {
        if (output != nullptr || testSink != nullptr || deviceName.isEmpty()) return;
        if (nowMs - lastOpenAttemptMs < 2000.0) return;     // plugged in later? keep trying, gently
        lastOpenAttemptMs = nowMs;

        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == deviceName)
            {
                output = juce::MidiOutput::openDevice (d.identifier);
                sentRed.fill (-1);
                sentGreen.fill (-1);
                break;
            }
    }

    void PadLights::sendLed (int hw, int red, int green, bool force)
    {
        auto emit = [this] (int cc, int value)
        {
            const auto m = juce::MidiMessage::controllerEvent (kChannel, cc, juce::jlimit (0, 127, value));
            if (testSink != nullptr) testSink->send (m);
            else if (output != nullptr) output->sendMessageNow (m);
        };
        if (force || sentRed[(size_t) hw] != red)     { emit (ccFor (hw, true,  swapColours), red);   sentRed[(size_t) hw] = red; }
        if (force || sentGreen[(size_t) hw] != green) { emit (ccFor (hw, false, swapColours), green); sentGreen[(size_t) hw] = green; }
    }

    void PadLights::sendAllOff()
    {
        if (output == nullptr && testSink == nullptr) return;
        for (int hw = 0; hw < kNumPads; ++hw)
            sendLed (hw, 0, 0, true);
    }

    void PadLights::tick (double nowMs)
    {
        const juce::ScopedLock sl (lock);
        tryOpen (nowMs);
        if (output == nullptr && testSink == nullptr) return;

        const bool fullRefresh = nowMs - lastFullRefreshMs > 4000.0;
        if (fullRefresh) lastFullRefreshMs = nowMs;
        const bool blinkOn = std::fmod (nowMs, 250.0) < 125.0;     // 4 Hz

        std::array<int, kNumPads> red {}, green {};

        if (testStartMs >= 0.0)
        {
            // TEST: all red, all green, both (amber), then back to normal.
            const double t = nowMs - testStartMs;
            const int phase = (int) (t / 700.0);
            if (phase >= 3) testStartMs = -1.0;
            else { red.fill (phase != 1 ? 127 : 0); green.fill (phase != 0 ? 127 : 0); }
            if (testStartMs >= 0.0)
            {
                for (int hw = 0; hw < kNumPads; ++hw) sendLed (hw, red[(size_t) hw], green[(size_t) hw], fullRefresh);
                return;
            }
        }

        if (learning)
        {
            // LEARN: the hardware pad being learned glows red; hit it.
            const auto count = bridge.midiEventCount.load();
            if (count != learnMidiCount)
            {
                learnMidiCount = count;
                const int pad = engine.padForNote (bridge.lastMidiNote.load());
                if (pad >= 0)
                {
                    for (auto& v : learned) if (v == learnStep) v = -1;
                    learned[(size_t) pad] = learnStep;
                }
                if (++learnStep >= kNumPads)
                {
                    learning = false;
                    triggerAsyncUpdate();       // layout learned: save it with the set
                }
            }
            if (learning)
            {
                for (int hw = 0; hw < kNumPads; ++hw)
                    sendLed (hw, hw == learnStep ? 127 : 0, 0, fullRefresh);
                return;
            }
        }

        // Normal: one hardware pad per plugin pad.
        for (int p = 0; p < kNumPads; ++p)
        {
            int hw = learned[(size_t) p];
            if (hw < 0)
            {
                const int note = engine.getPadNote (p);
                for (int k = 0; k < kNumPads; ++k)
                    if (kDefaultNotes[k] == note) { hw = k; break; }
            }
            if (hw < 0) continue;

            const float act = bridge.padActivity[(size_t) p].load (std::memory_order_relaxed);
            if (bridge.padQueued[(size_t) p].load (std::memory_order_relaxed))
            {
                red[(size_t) hw] = blinkOn ? 127 : 0;
            }
            else if (act > 0.02f)
            {
                green[(size_t) hw] = 24 + juce::roundToInt (103.0f * juce::jmin (1.0f, act));
            }
            else if (bridge.padLoaded[(size_t) p].load (std::memory_order_relaxed))
            {
                green[(size_t) hw] = 6;
            }
        }

        for (int hw = 0; hw < kNumPads; ++hw)
            sendLed (hw, red[(size_t) hw], green[(size_t) hw], fullRefresh);
    }
}
