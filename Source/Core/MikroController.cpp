#include "Core/MikroController.h"
#include "Core/Parameters.h"
#include "Core/FxDefs.h"
#include "PluginProcessor.h"
#include <mutex>

namespace tb
{
    using namespace mikro;

    namespace
    {
        // Modifier keys (a pad pressed with one of these doesn't play).
        uint32_t modBit (int b) noexcept
        {
            switch (b)
            {
                case shift:     return 1u << 0;
                case mikro::select:    return 1u << 1;
                case mute:      return 1u << 2;
                case solo:      return 1u << 3;
                case duplicate: return 1u << 4;
                case erase:     return 1u << 5;
                default:        return 0u;
            }
        }

        const char* const kStepFields[] = { "VEL", "CHANCE", "RATCHET", "PITCH", "SLICE", "MICRO", "REV", "COND" };
        constexpr int kStepDivisor[]    = { 1, 1, 4, 3, 3, 1, 3, 3 };
        // The order the encoder push walks them on the Mikro: SLICE first (A9: building breaks).
        constexpr int kFieldOrder[]     = { 4, 0, 1, 2, 3, 5, 6, 7 };
        const char* const kRollNames[]  = { "1/8", "1/16", "1/32" };

        float velocityOf (int value) noexcept { return (float) juce::jlimit (1, 127, value >> 5) / 127.0f; }
    }

    //==========================================================================
    //  MikroHub
    //==========================================================================
    std::shared_ptr<MikroHub> MikroHub::shared()
    {
        static std::mutex m;
        static std::weak_ptr<MikroHub> w;
        std::lock_guard<std::mutex> g (m);
        if (auto s = w.lock()) return s;
        auto s = std::make_shared<MikroHub> (true);
        w = s;
        return s;
    }

    MikroHub::MikroHub (bool open) : juce::Thread ("Mikro MK3 connect"), openDevice (open)
    {
        usb.onReport  = [this] (const uint8_t* d, int n) { dispatch (d, n); };
        usb.onRemoved = [this] { removed = true; notify(); };
        if (openDevice && MikroUsb::isSupported()) startThread();
        else
        {
            const juce::ScopedLock sl (lock);
            status = MikroUsb::isSupported() ? "test" : "the Mikro MK3 connection is Mac only";
        }
    }

    MikroHub::~MikroHub()
    {
        signalThreadShouldExit();
        notify();
        stopThread (5000);
        const juce::ScopedLock dl (deviceLock);
        usb.close();
    }

    void MikroHub::run()
    {
        double lastAttempt = -1.0e9;
        while (! threadShouldExit())
        {
            if (removed.exchange (false))
            {
                const juce::ScopedLock dl (deviceLock);
                usb.close();
                connected = false;
                const juce::ScopedLock sl (lock);
                status = "unplugged - plug it back in";
            }
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (! connected.load() && now - lastAttempt >= 2000.0)
            {
                lastAttempt = now;
                bool ok;
                juce::String s;
                {
                    const juce::ScopedLock dl (deviceLock);
                    ok = usb.open();
                    s = ok ? usb.describe() : usb.getLastError();
                }
                {
                    const juce::ScopedLock sl (lock);
                    status = s;
                }
                if (ok) { writeFailures = 0; connected = true; connections.fetch_add (1); }
            }
            if (connected.load())
            {
                // Send the newest lights / screen (written here, so the UI never waits on USB).
                std::array<std::vector<uint8_t>, 3> todo;
                {
                    const juce::ScopedLock ol (outLock);
                    for (size_t k = 0; k < 3; ++k) if (outDirty[k]) { todo[k] = outSlots[k]; outDirty[k] = false; }
                }
                for (auto& r : todo)
                {
                    if (r.empty()) continue;
                    bool ok;
                    {
                        const juce::ScopedLock dl (deviceLock);
                        ok = usb.write (r);
                    }
                    writeFailures = ok ? 0 : writeFailures + 1;
                }
                if (writeFailures >= 5)
                {
                    // The Mikro refuses us (NI's Maschine app took it?): drop it, keep trying.
                    const juce::ScopedLock dl (deviceLock);
                    usb.close();
                    connected = false;
                    const juce::ScopedLock sl (lock);
                    status = "the Mikro stopped answering (is NI's Maschine app using it?)";
                }
            }
            wait (connected.load() ? 20 : 500);
        }
    }

    juce::String MikroHub::getStatus() const
    {
        const juce::ScopedLock sl (lock);
        return status;
    }

    void MikroHub::attach (MikroController* c)
    {
        const juce::ScopedLock sl (lock);
        if (std::find (attached.begin(), attached.end(), c) == attached.end()) attached.push_back (c);
        if (owner == nullptr) owner = c;
    }

    void MikroHub::detach (MikroController* c)
    {
        const juce::ScopedLock sl (lock);
        attached.erase (std::remove (attached.begin(), attached.end(), c), attached.end());
        if (owner == c) owner = attached.empty() ? nullptr : attached.back();
    }

    void MikroHub::claim (MikroController* c)
    {
        const juce::ScopedLock sl (lock);
        if (std::find (attached.begin(), attached.end(), c) != attached.end()) owner = c;
    }

    bool MikroHub::isOwner (const MikroController* c) const
    {
        const juce::ScopedLock sl (lock);
        return c != nullptr && owner == c;
    }

    void MikroHub::send (const MikroController* from, const std::vector<uint8_t>& report)
    {
        if (! isOwner (from) || report.empty()) return;
        if (testSink != nullptr) { testSink->send (report); return; }
        const size_t slot = report[0] == 0x80 ? 0 : (report.size() > 3 && report[3] == 0x02 ? 2 : 1);
        {
            const juce::ScopedLock ol (outLock);
            outSlots[slot] = report;
            outDirty[slot] = true;
        }
        notify();
    }

    void MikroHub::dispatch (const uint8_t* data, int size)
    {
        const juce::ScopedLock sl (lock);
        if (owner != nullptr) owner->handleReport (data, size);
    }

    void MikroHub::setSinkForTesting (Sink* s)
    {
        testSink = s;
        connected = s != nullptr;
        connections.fetch_add (1);
    }

    //==========================================================================
    //  MikroController
    //==========================================================================
    const juce::Identifier MikroController::mikroId { "MIKRO" };

    MikroController::MikroController (TwistedBreaksProcessor& p)
        : processor (p), bridge (p.getBridge()), apvts (p.getParameters())
    {
        sceneViewRaw = apvts.getRawParameterValue (params::sceneView);
        seqLengthRaw = apvts.getRawParameterValue (params::seqLength);
        heldPlay.fill (-1);
    }

    MikroController::~MikroController()
    {
        stopTimer();
        if (hub != nullptr && isActive())
        {
            lights.clear();
            hub->send (this, lights.report());
            screen.clear();
            for (auto& r : screen.reports()) hub->send (this, r);
        }
        detachHub();
    }

    void MikroController::attachHub()
    {
        if (remote == nullptr) remote = RemotePort::shared();
        if (hub == nullptr) hub = MikroHub::shared();
        hub->attach (this);
        hub->claim (this);
    }

    void MikroController::detachHub()
    {
        if (hub == nullptr) return;
        hub->detach (this);          // the USB thread never calls us after this
        releaseHeldPlays();
        hub.reset();
        remote.reset();
    }

    void MikroController::setEnabled (bool on)
    {
        if (on == enabled) return;
        enabled = on;
        if (on)
        {
            attachHub();
            lightsKnown = screenKnown = false;
            if (! testing) startTimerHz (50);
        }
        else
        {
            stopTimer();
            if (hub != nullptr && isConnected() && isOwner())
            {
                lights.clear();
                hub->send (this, lights.report());
                screen.clear();
                for (auto& r : screen.reports()) hub->send (this, r);
            }
            releaseEverything();
            detachHub();
            wasActive = false;
        }
        if (onSettingsChanged) onSettingsChanged();
    }

    void MikroController::claim()
    {
        if (hub != nullptr && ! hub->isOwner (this))
        {
            hub->claim (this);
            resetInput = true;
            lightsKnown = screenKnown = false;
        }
    }

    bool MikroController::isConnected() const { return hub != nullptr && hub->isConnected(); }
    bool MikroController::isOwner() const     { return hub != nullptr && hub->isOwner (this); }
    juce::String MikroController::getHubStatus() const { return hub != nullptr ? hub->getStatus() : juce::String ("off"); }

    juce::ValueTree MikroController::toValueTree() const
    {
        juce::ValueTree t (mikroId);
        t.setProperty ("enabled", enabled, nullptr);
        t.setProperty ("mode", modeAtomic.load(), nullptr);
        return t;
    }

    void MikroController::fromValueTree (const juce::ValueTree& t)
    {
        if (! t.isValid()) return;
        modeAtomic = juce::jlimit (0, 4, (int) t.getProperty ("mode", 0));
        shownMode = (Mode) modeAtomic.load();
        setEnabled ((bool) t.getProperty ("enabled", false));
    }

    void MikroController::useHubForTesting (std::shared_ptr<MikroHub> h)
    {
        testing = true;
        stopTimer();
        if (hub != nullptr) detachHub();
        hub = std::move (h);
        enabled = true;
        attachHub();
        lightsKnown = screenKnown = false;
    }

    void MikroController::timerCallback()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        processPending (now);
        tick (now);
    }

    const char* MikroController::modeName (Mode m) noexcept
    {
        switch (m)
        {
            case Mode::pads:     return "PADS";
            case Mode::steps:    return "STEPS";
            case Mode::keyboard: return "SLICES";
            case Mode::perform:  return "PERFORM";
            case Mode::scene:    return "SNAPSHOTS";
        }
        return "";
    }

    bool MikroController::wholeOnly (int pad) const
    {
        return raw (params::padId (pad, params::padStepsWhole)) > 0.5f;
    }

    void MikroController::setSelectedPad (int pad)
    {
        pad = juce::jlimit (0, tb::kNumPads - 1, pad);
        if (pad != chosenFor) { chosenSlice = 0; chosenFor = pad; }      // a new pad starts on WHOLE (A10)
        if (pad != selectedPad.load()) heldSteps.clear();
        selectedPad = pad;
    }

    void MikroController::selectPad (int pad)
    {
        pad = ((pad % tb::kNumPads) + tb::kNumPads) % tb::kNumPads;
        if (pad != selectedPad.load()) heldSteps.clear();
        if (pad != chosenFor) { chosenSlice = 0; chosenFor = pad; }      // a new pad starts on WHOLE (A10)
        selectedPad = pad;
        ++selectionVersion;
        processor.getMutator().setSelectedPad (pad);
        processor.getResampler().setSelectedPad (pad);
        if (sceneOf (pad) != juce::roundToInt (sceneViewRaw->load()))
            setParamPlain (params::sceneView, (float) sceneOf (pad));
    }

    // ==========================================================================
    //  USB thread
    // ==========================================================================
    void MikroController::releaseHeld (int i)
    {
        const int pad = heldPlay[(size_t) i];
        if (pad < 0) return;
        if (heldRoll[(size_t) i]) bridge.mikroEvents.push ({ ControllerHit::rollOff, (int8_t) pad, 0, 0, 0.0f });
        if (! heldSlice[(size_t) i]) bridge.mikroEvents.push ({ ControllerHit::padOff, (int8_t) pad, 0, 0, 0.0f });
        bridge.padPressure[(size_t) pad].store (0.0f);
        heldPlay[(size_t) i] = -1;
        heldRoll[(size_t) i] = heldSlice[(size_t) i] = false;
        heldMask.fetch_and (~(1u << i));
    }

    void MikroController::releaseHeldPlays()
    {
        for (int i = 0; i < 16; ++i) releaseHeld (i);
        usbMods = 0;
        usbRepeatHeld = false;
    }

    void MikroController::handleReport (const uint8_t* data, int size)
    {
        if (resetInput.exchange (false))
        {
            // A new owner / a reconnect: start from what the Mikro says now (no stale
            // encoder position, no keys we never saw released).
            releaseHeldPlays();
            parser = {};
        }
        std::vector<Event> evs;
        parser.parse (data, size, evs);
        constexpr uint32_t noPlay = 0x3f;

        for (const auto& e : evs)
        {
            if (e.kind == Event::button)
            {
                if (const uint32_t bit = modBit (e.index)) usbMods = e.down ? (usbMods | bit) : (usbMods & ~bit);
                if (e.down)
                {
                    // Decided here, so the very next pad press already follows them.
                    switch (e.index)
                    {
                        case padMode:
                            padTemp = getMode() == Mode::steps;          // held from STEP: pick the pad to sequence
                            padUsed = false;
                            padDownMs = juce::Time::getMillisecondCounterHiRes();
                            modeAtomic = (int) Mode::pads;
                            break;
                        case keyboard:
                            if ((usbMods & 1u) != 0) break;              // SHIFT + KEYBOARD: clear the chosen slice (message thread)
                            kbdTemp = getMode() == Mode::steps;          // held from STEP: a quick look at the slices
                            kbdUsed = false;
                            kbdDownMs = juce::Time::getMillisecondCounterHiRes();
                            modeAtomic = (int) Mode::keyboard;
                            break;
                        case step:     modeAtomic = (int) Mode::steps; break;
                        case perform:  modeAtomic = (int) Mode::perform; break;
                        case scene:    modeAtomic = (int) Mode::scene; break;
                        case fixedVel: fixedVelOn = ! fixedVelOn.load(); break;
                        case noteRepeat: if ((usbMods & 1u) != 0) rollRate = (rollRate.load() + 1) % 3; break;
                        default: break;
                    }
                }
                if (e.index == padMode && ! e.down && padTemp)
                {
                    if (padUsed || juce::Time::getMillisecondCounterHiRes() - padDownMs > 400.0) modeAtomic = (int) Mode::steps;
                    padTemp = false;
                }
                if (e.index == keyboard && ! e.down && kbdTemp)
                {
                    // Let go after choosing a slice (or a long hold): back to the steps.
                    if (kbdUsed || juce::Time::getMillisecondCounterHiRes() - kbdDownMs > 400.0) modeAtomic = (int) Mode::steps;
                    kbdTemp = false;
                }
                if (e.index == noteRepeat && (e.down ? (usbMods & 1u) == 0 : true))
                {
                    usbRepeatHeld = e.down;
                    // Pads already held start / stop rolling with the key.
                    for (int i = 0; i < 16; ++i)
                    {
                        const int pad = heldPlay[(size_t) i];
                        if (pad < 0) continue;
                        if (e.down && ! heldRoll[(size_t) i])
                        {
                            const int slice = heldSlice[(size_t) i] ? padNumberOf (i) + 1 : 0;
                            bridge.mikroEvents.push ({ ControllerHit::rollOn, (int8_t) pad, (uint8_t) slice, (uint8_t) rollRate.load(), -heldVel[(size_t) i] });
                            heldRoll[(size_t) i] = true;
                        }
                        else if (! e.down && heldRoll[(size_t) i])
                        {
                            bridge.mikroEvents.push ({ ControllerHit::rollOff, (int8_t) pad, 0, 0, 0.0f });
                            heldRoll[(size_t) i] = false;
                        }
                    }
                }
                inbox.push (e);
                continue;
            }
            if (e.kind != Event::pad) { inbox.push (e); continue; }

            const int i = e.index;
            if (e.padEvent == 0x40)
            {
                // Pressure (also sent while just touching: only for a pad that's playing).
                if (heldPlay[(size_t) i] >= 0) bridge.padPressure[(size_t) heldPlay[(size_t) i]].store ((float) e.value / 4095.0f);
                continue;
            }
            const bool down = e.padEvent == 0x10, up = e.padEvent == 0x20 || e.padEvent == 0x30;
            if (! down && ! up) continue;

            const auto mode = getMode();
            const float vel = fixedVelOn.load() ? 1.0f : velocityOf (e.value);
            if (down && (usbMods & noPlay) == 0 && ! browsing.load() && (mode == Mode::pads || (mode == Mode::keyboard && selectedHasSlices.load())))
            {
                releaseHeld (i);                       // (a missed release of this pad: start it clean)
                int pad, slice = 0;
                if (mode == Mode::pads)
                {
                    const int view = juce::jlimit (0, kNumScenes - 1, juce::roundToInt (sceneViewRaw->load()));
                    pad = view * kPadsPerScene + padNumberOf (i);   // pad 1 (bottom left) = 01
                    selectedPad = pad;
                    padUsed = true;                 // (the message thread does the rest of selecting)
                    bridge.padPressure[(size_t) pad].store (0.0f);
                    bridge.mikroEvents.push ({ ControllerHit::padOn, (int8_t) pad, 0, 0, vel });
                }
                else
                {
                    pad = selectedPad.load();
                    slice = padNumberOf (i) + 1;
                    kbdUsed = true;
                    bridge.mikroEvents.push ({ ControllerHit::sliceOn, (int8_t) pad, (uint8_t) slice, 0, vel });
                }
                heldPlay[(size_t) i] = pad;
                heldSlice[(size_t) i] = slice > 0;
                heldVel[(size_t) i] = vel;
                heldMask.fetch_or (1u << i);
                if (usbRepeatHeld)
                {
                    bridge.mikroEvents.push ({ ControllerHit::rollOn, (int8_t) pad, (uint8_t) slice, (uint8_t) rollRate.load(), vel });
                    heldRoll[(size_t) i] = true;
                }
            }
            else if (up) releaseHeld (i);              // released as it was pressed (pad or slice)

            Event q = e;
            q.index = i;              // Mikro pad number (0 = bottom left)
            q.down = down;
            inbox.push (q);
        }
    }

    // ==========================================================================
    //  Message thread
    // ==========================================================================
    juce::RangedAudioParameter* MikroController::param (const juce::String& id) const { return apvts.getParameter (id); }

    float MikroController::raw (const juce::String& id) const
    {
        if (auto* v = apvts.getRawParameterValue (id)) return v->load();
        return 0.0f;
    }

    void MikroController::setParamPlain (const juce::String& id, float plain)
    {
        if (auto* p = param (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
            p->endChangeGesture();
        }
    }

    int MikroController::groupOf (int pad) const
    {
        return juce::jlimit (0, kNumGroups - 1, juce::roundToInt (raw (params::padId (pad, params::padGroup))));
    }

    int MikroController::laneLength (int pad) const
    {
        const int global = juce::jlimit (1, SequencerData::kSteps, juce::roundToInt (seqLengthRaw->load()));
        const int own = juce::roundToInt (raw (params::padId (pad, params::padSeqLen)));
        return params::isSeqMode (juce::roundToInt (raw (params::padId (pad, params::padMode)))) && own > 0 ? juce::jmin (own, 32) : global;
    }

    void MikroController::flash (const juce::String& title, const juce::String& value, double nowMs)
    {
        flashTitle = title;
        flashValue = value;
        flashUntilMs = nowMs + 1200.0;
    }

    void MikroController::endGesture()
    {
        if (gestureParam != nullptr) gestureParam->endChangeGesture();
        gestureParam = nullptr;
    }

    void MikroController::setFill (bool on)
    {
        if (on == fillHeld) return;
        fillHeld = on;
        setParamPlain (params::seqFill, on ? 1.0f : 0.0f);
    }

    void MikroController::setMode (Mode m)
    {
        // (the USB thread already switched modeAtomic: tidy up after the mode we leave)
        modeAtomic = (int) m;
        if (m == shownMode) return;
        for (int f = 0; f < 16; ++f) toggleFx (f, false);
        heldSteps.clear();
        endGesture();
        shownMode = m;
        if (onSettingsChanged) onSettingsChanged();
    }

    void MikroController::setPage (Page p)
    {
        endGesture();
        encoderAcc = 0;
        page = p;
    }

    void MikroController::toggleFx (int f, bool down)
    {
        if (! juce::isPositiveAndBelow (f, 16)) return;
        auto& heldId = fxHeldId[(size_t) f];
        if (! down)
        {
            if (heldId.isNotEmpty()) setParamPlain (heldId, 0.0f);     // what this press switched on, whatever changed since
            heldId = {};
            return;
        }
        const auto id = fx::paramId (groupOf (selectedPad.load()), f);
        if (processor.isFxLocked (f)) setParamPlain (id, raw (id) > 0.5f ? 0.0f : 1.0f);
        else if (raw (id) < 0.5f)     { setParamPlain (id, 1.0f); heldId = id; }
    }

    std::vector<MikroController::Param> MikroController::pageParams() const
    {
        std::vector<Param> v;
        const int pad = selectedPad.load();
        auto padP = [&v, pad] (const char* name, const char* label) { v.push_back ({ params::padId (pad, name), label }); };
        const Page pg = getMode() == Mode::perform ? Page::fx : page;
        switch (pg)
        {
            case Page::pad:
                padP (params::padGain, "GAIN");          padP (params::padPan, "PAN");
                padP (params::padPitch, "PITCH");        padP (params::padStart, "START");
                padP (params::padEnd, "END");            padP (params::padSlice, "SLICE");
                padP (params::padCharacter, "CHARACTER"); padP (params::padPress, "PRESSURE");
                padP (params::padVelSens, "VEL SENS");   padP (params::padChoke, "CHOKE");
                padP (params::padMode, "MODE");          padP (params::padLaunch, "LAUNCH");
                padP (params::padSeqLen, "SEQ LEN");     padP (params::padSwap, "SWAP");
                break;
            case Page::seq:
                v.push_back ({ params::seqRes, "STEP" });        v.push_back ({ params::seqLength, "LEN" });
                v.push_back ({ params::padId (pad, params::padSeqLen), "PAD LEN" });
                v.push_back ({ params::padId (pad, params::padLaunch), "LAUNCH" });
                v.push_back ({ params::mutAmount, "MUTATE" });   v.push_back ({ params::mutAuto, "MUTATE AUTO" });
                v.push_back ({ params::mutAt, "MUTATE AT" });    v.push_back ({ params::seqQuant, "QUANTISE" });
                break;
            case Page::mix:
                for (int g = 0; g < kNumGroups; ++g)
                    v.push_back ({ params::groupId (g, params::grpLevel), "GROUP " + juce::String::charToString ((juce::juce_wchar) ('A' + g)) });
                v.push_back ({ params::xfade, "CROSSFADER" });   v.push_back ({ params::xfadeCurve, "CURVE" });
                v.push_back ({ params::macroAmount, "MACRO AMOUNT" }); v.push_back ({ params::masterGain, "MASTER" });
                break;
            case Page::fx:
            {
                const auto& d = fx::def (lastFx);
                for (int k = 0; k < d.numParams; ++k)
                    v.push_back ({ fx::paramId (groupOf (pad), lastFx, k), juce::String (d.label) + " " + d.params[k].label });
                break;
            }
        }
        return v;
    }

    juce::String MikroController::paramLabel() const
    {
        if (getMode() == Mode::steps && ! heldSteps.empty())
            return "STEP " + juce::String (heldSteps.front().step + 1) + " " + kStepFields[kFieldOrder[stepField]];
        const auto v = pageParams();
        if (v.empty()) return juce::String (fx::def (lastFx).label) + ": NO SETTINGS";
        const Page pg = getMode() == Mode::perform ? Page::fx : page;
        return v[(size_t) (paramIndex[(size_t) pg] % (int) v.size())].label;
    }

    juce::String MikroController::paramValue() const
    {
        if (getMode() == Mode::steps && ! heldSteps.empty())
        {
            const auto st = bridge.seq.get (selectedPad.load(), heldSteps.front().step);
            switch (kFieldOrder[stepField])
            {
                case 0: return juce::String (st.velocity);
                case 1: return juce::String (st.probability) + "%";
                case 2: return "X" + juce::String (st.ratchet);
                case 3: return (st.pitch > 0 ? "+" : "") + juce::String (st.pitch) + " ST";
                case 4: return st.slice == 0 || wholeOnly (selectedPad.load()) ? juce::String ("WHOLE PAD") : juce::String (st.slice);
                case 5: return (st.micro > 0 ? "+" : "") + juce::String (st.micro);
                case 6: return st.rev ? "ON" : "OFF";
                default: return kSeqCondNames[juce::jlimit (0, kNumSeqConds - 1, st.cond)];
            }
        }
        const auto v = pageParams();
        if (v.empty()) return {};
        const Page pg = getMode() == Mode::perform ? Page::fx : page;
        if (auto* p = param (v[(size_t) (paramIndex[(size_t) pg] % (int) v.size())].id)) return p->getCurrentValueAsText().toUpperCase();
        return {};
    }

    void MikroController::editStep (int units)
    {
        const int pad = selectedPad.load();
        for (auto& h : heldSteps)
        {
            auto st = bridge.seq.get (pad, h.step);
            st.on = true;
            switch (kFieldOrder[stepField])
            {
                case 0: st.velocity    = juce::jlimit (1, 127, st.velocity + units); break;
                case 1: st.probability = juce::jlimit (0, 100, st.probability + units); break;
                case 2: st.ratchet     = juce::jlimit (1, 4, st.ratchet + units); break;
                case 3: st.pitch       = juce::jlimit (-24, 24, st.pitch + units); break;
                case 4: st.slice       = juce::jlimit (0, 16, st.slice + units); break;
                case 5: st.micro       = juce::jlimit (-64, 63, st.micro + units); break;
                case 6: st.rev         = units > 0; break;
                default: st.cond       = juce::jlimit (0, kNumSeqConds - 1, st.cond + units); break;
            }
            bridge.seq.set (pad, h.step, st);
            h.edited = true;
        }
        processor.markStateChanged();
    }

    void MikroController::turnEncoder (int ticks, double nowMs)
    {
        lastTurnMs = nowMs;
        if (browsing.load())
        {
            browseAcc += ticks;
            const int units = browseAcc / 2;
            browseAcc -= units * 2;
            if (units != 0) processor.getPresets().moveCursor (units);
            return;
        }
        if (getMode() == Mode::steps && ! heldSteps.empty())
        {
            encoderAcc += ticks;
            const int div = kStepDivisor[kFieldOrder[stepField]];
            const int units = encoderAcc / div;
            encoderAcc -= units * div;
            if (units != 0) editStep (units);
            return;
        }
        const auto v = pageParams();
        if (v.empty()) return;
        const Page pg = getMode() == Mode::perform ? Page::fx : page;
        auto& idx = paramIndex[(size_t) pg];
        if (held[shift])
        {
            // SHIFT + turn: the next / previous parameter.
            endGesture();
            idx = ((idx % (int) v.size()) + (ticks > 0 ? 1 : -1) + (int) v.size()) % (int) v.size();
            return;
        }
        auto* p = param (v[(size_t) (idx % (int) v.size())].id);
        if (p == nullptr) return;
        if (gestureParam != p) { endGesture(); p->beginChangeGesture(); gestureParam = p; }
        if (p->isDiscrete())
        {
            encoderAcc += ticks;
            const int units = encoderAcc / 2;
            encoderAcc -= units * 2;
            if (units == 0) return;
            const auto range = p->getNormalisableRange();
            const float plain = juce::jlimit (range.start, range.end, (float) juce::roundToInt (p->convertFrom0to1 (p->getValue())) + (float) units);
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
        }
        else p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->getValue() + (float) ticks / 64.0f));
    }

    void MikroController::pushEncoder()
    {
        endGesture();
        encoderAcc = 0;
        if (browsing.load())
        {
            // A11: push = load (pads onto the selected pad); SHIFT + push = listen.
            auto& pm = processor.getPresets();
            const auto& list = pm.getList();
            if (! juce::isPositiveAndBelow (pm.getCursor(), list.size())) return;
            const auto f = list[pm.getCursor()].file;
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (held[shift]) { pm.preview (f); flash ("PREVIEW", list[pm.getCursor()].name.toUpperCase(), now); return; }
            const auto r = pm.load (f, selectedPad.load());
            flash (r.wasOk() ? "LOADING" : "CAN'T LOAD", r.wasOk() ? list[pm.getCursor()].name.toUpperCase() : r.getErrorMessage().toUpperCase(), now);
            return;
        }
        if (getMode() == Mode::steps && ! heldSteps.empty()) { stepField = (stepField + 1) % 8; return; }
        const auto v = pageParams();
        if (v.empty()) return;
        const Page pg = getMode() == Mode::perform ? Page::fx : page;
        auto& idx = paramIndex[(size_t) pg];
        idx = ((idx % (int) v.size()) + (held[shift] ? -1 : 1) + (int) v.size()) % (int) v.size();
    }

    void MikroController::handleStrip (int value)
    {
        if (value == 0)
        {
            if (stripParam != nullptr) stripParam->endChangeGesture();
            stripParam = nullptr;
            return;
        }
        if (stripParam == nullptr)
        {
            stripParam = param (held[shift] ? params::macroAmount : params::xfade);
            if (stripParam == nullptr) return;
            stripParam->beginChangeGesture();
        }
        stripParam->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) (value - 1) / 199.0f));
    }

    void MikroController::handleButton (int b, bool down, double nowMs)
    {
        held[(size_t) b] = down;
        setMode (getMode());                    // (the USB thread already chose it: tidy up after the old one)

        // PLAY / STOP / REC drive Live's transport through the "Twisted Breaks Remote" port (A9).
        if (remote != nullptr && (b == play || b == stop || b == rec))
            remote->key (b == play ? RemotePort::play : b == stop ? RemotePort::stop : RemotePort::record, down);

        if (! down)
        {
            if (b == tap) setFill (false);
            if (b == duplicate) dupSource = -1;
            if (b == encoderTouch && nowMs - lastTurnMs > 0.0) endGesture();
            return;
        }
        const bool sh = held[shift];
        const int view = juce::roundToInt (sceneViewRaw->load());
        switch (b)
        {
            case keyboard:
                if (sh) { chosenSlice = 0; flash ("SLICE", "WHOLE PAD", nowMs); }
                break;
            case plugin:    setPage (Page::pad); break;
            case swing:     setPage (Page::seq); break;
            case volume:    setPage (Page::mix); break;
            case group:
            {
                const int other = 1 - view;
                setParamPlain (params::sceneView, (float) other);
                selectPad (other * kPadsPerScene + selectedPad.load() % kPadsPerScene);
                flash ("SCENE", other == 0 ? "A" : "B", nowMs);
                break;
            }
            case left:
            case right:
                if (getMode() == Mode::steps) { stepPage = b == left ? 0 : 1; heldSteps.clear(); flash ("STEPS", stepPage == 0 ? "1-16" : "17-32", nowMs); }
                else selectPad (selectedPad.load() + (b == left ? -1 : 1));
                break;
            case fixedVel:                      // (toggled on the USB thread)
                flash ("FIXED VEL", fixedVelOn.load() ? "ON" : "OFF", nowMs);
                break;
            case variation:
                if (sh) { if (processor.getMutator().canUndo()) processor.getMutator().undo(); flash ("MUTATE", "UNDO", nowMs); }
                else    { processor.getMutator().mutate(); flash ("MUTATE", "GO", nowMs); }
                break;
            case tap:       setFill (true); flash ("FILL", "HELD", nowMs); break;
            case sampling:
            {
                auto& rs = processor.getResampler();
                rs.setSelectedPad (selectedPad.load());
                if (rs.getState() == Resampler::State::idle) { rs.record(); flash ("RESAMPLE", "REC", nowMs); }
                else                                         { rs.cancel(); flash ("RESAMPLE", "CANCEL", nowMs); }
                break;
            }
            case noteRepeat:                    // (SHIFT: the rate already stepped on the USB thread)
                flash ("NOTE REPEAT", kRollNames[rollRate.load()], nowMs);
                break;
            case encoderPress: pushEncoder(); break;
            case browse:
                browsing = ! browsing.load();
                browseAcc = 0;
                if (browsing.load()) { processor.getPresets().rescan(); releaseHeldPlays(); }
                break;
            default: break;
        }
    }

    void MikroController::handlePad (int i, bool down, double nowMs)
    {
        const int pos = i;                        // reading order (FX grid, as on screen)
        const int num = padNumberOf (i);          // pad number, 0 = pad 1 bottom left (pads, slices, snapshots, steps)
        const auto mode = getMode();
        const int view = juce::jlimit (0, kNumScenes - 1, juce::roundToInt (sceneViewRaw->load()));
        auto& seq = bridge.seq;
        const int sel = selectedPad.load();

        // A11: BROWSE: pads 1-4 pick what the list shows (kits / pads / sequences / favourites).
        if (browsing.load())
        {
            if (down && num < 4)
            {
                using F = PresetManager::Filter;
                const F f[] = { F::kits, F::pads, F::sequences, F::favourites };
                processor.getPresets().setFilter (f[num]);
                processor.getPresets().setCursor (0);
            }
            return;
        }

        // A11: SHIFT + pad 1 / 2 = UNDO / REDO, pad 11 / 12 = COPY / PASTE the selected pad
        // (as printed under the pads in Maschine; not in SCENE, where SHIFT stores).
        if (down && held[shift] && ! held[duplicate] && ! held[erase] && mode != Mode::scene && mode != Mode::perform)
        {
            auto& ops = processor.getPadOps();
            switch (num)
            {
                case 0:  { const auto n = ops.undoName(); flash ("UNDO", ops.undo() ? n : juce::String ("NOTHING"), nowMs); return; }
                case 1:  { const auto n = ops.redoName(); flash ("REDO", ops.redo() ? n : juce::String ("NOTHING"), nowMs); return; }
                case 10: ops.copy (sel); flash ("COPY", padLabel (sel), nowMs); return;
                case 11: flash ("PASTE", ops.paste (sel) ? padLabel (ops.clipSource()) + " > " + padLabel (sel) : juce::String ("EMPTY"), nowMs); return;
                default: break;
            }
        }

        switch (mode)
        {
            case Mode::pads:
            {
                if (! down) return;
                const int pad = view * kPadsPerScene + num;
                auto toggle = [this, pad] (const char* name)
                {
                    const auto id = params::padId (pad, name);
                    setParamPlain (id, raw (id) > 0.5f ? 0.0f : 1.0f);
                };
                if      (held[mute])      { toggle (params::padMute); flash ("MUTE", padLabel (pad), nowMs); }
                else if (held[solo])      { toggle (params::padSolo); flash ("SOLO", padLabel (pad), nowMs); }
                else if (held[duplicate])
                {
                    // A11: hold DUPLICATE, hit the pad to copy, then the pad to copy onto
                    // (SHIFT + DUPLICATE: swap them).
                    if (dupSource < 0) { dupSource = pad; flash (held[shift] ? "SWAP" : "COPY", padLabel (pad) + " >", nowMs); }
                    else if (pad != dupSource)
                    {
                        auto& ops = processor.getPadOps();
                        if (held[shift]) { ops.swap (dupSource, pad); flash ("SWAP", padLabel (dupSource) + " < > " + padLabel (pad), nowMs); }
                        else             { ops.copyPad (dupSource, pad); flash ("COPY", padLabel (dupSource) + " > " + padLabel (pad), nowMs); }
                        dupSource = -1;
                    }
                }
                else if (held[erase] && held[shift]) { processor.getPadOps().clear (pad); flash ("CLEAR PAD", padLabel (pad), nowMs); }
                else if (held[erase]) { seq.clearLane (pad); processor.markStateChanged(); flash ("CLEAR LANE", padLabel (pad), nowMs); }
                else
                {
                    selectPad (pad);
                    chosenSlice = 0;                         // PAD MODE: steps play the whole pad (A10)
                    chosenFor = pad;
                }
                return;
            }
            case Mode::steps:
            {
                const int st = stepPage * 16 + num;   // step 1 = pad 1 (bottom left), step 16 top right
                if (down)
                {
                    if (held[erase]) { seq.set (sel, st, SeqStep {}); processor.markStateChanged(); return; }
                    if (! heldSteps.empty())
                    {
                        auto& first = heldSteps.front();
                        const auto src = seq.get (sel, first.step);
                        for (int k = juce::jmin (first.step, st); k <= juce::jmax (first.step, st); ++k) seq.set (sel, k, src);
                        first.edited = true;
                        heldSteps.push_back ({ st, false, true });
                        processor.markStateChanged();
                        return;
                    }
                    auto s = seq.get (sel, st);
                    const bool created = ! s.on;
                    // What the tap writes: the chosen slice, or the whole pad (PAD MODE pick, or a WHOLE ONLY pad).
                    const int choice = wholeOnly (sel) ? 0 : chosenSlice;
                    if (choice > 0 && (raw (params::padId (sel, params::padSwap)) > 0.5f || raw (params::padId (sel, params::padSliceSeq)) > 0.5f))
                        flash ("SLICES IGNORED", raw (params::padId (sel, params::padSwap)) > 0.5f ? "SWAP PAD" : "SLICE SEQ", nowMs);
                    if (created)
                    {
                        s = SeqStep {};
                        s.on = true;
                        s.slice = choice;                 // the chosen slice (0: the whole pad)
                        seq.set (sel, st, s);
                        processor.markStateChanged();
                    }
                    else if (s.slice != choice)
                    {
                        // A step of the other kind / another slice: it takes the choice (and stays on).
                        s.slice = choice;
                        seq.set (sel, st, s);
                        processor.markStateChanged();
                        heldSteps.push_back ({ st, false, true });
                        encoderAcc = 0;
                        return;
                    }
                    heldSteps.push_back ({ st, created, false });
                    encoderAcc = 0;
                }
                else
                {
                    auto it = std::find_if (heldSteps.begin(), heldSteps.end(), [st] (const HeldStep& h) { return h.step == st; });
                    if (it == heldSteps.end()) return;
                    if (! it->created && ! it->edited)
                    {
                        auto s = seq.get (sel, st);
                        s.on = false;
                        seq.set (sel, st, s);
                        processor.markStateChanged();
                    }
                    heldSteps.erase (it);
                }
                return;
            }
            case Mode::keyboard:
                if (down)
                {
                    if (! selectedHasSlices.load()) { flash ("NO SLICES", "SLICE THE PAD FIRST", nowMs); return; }
                    chosenSlice = num + 1;               // STEP writes this slice into the steps you tap (A9)
                    chosenFor = sel;
                    flash ("SLICE " + juce::String (chosenSlice), wholeOnly (sel) ? "WHOLE ONLY PAD" : "CHOSEN", nowMs);
                }
                return;
            case Mode::perform:
                if (down) lastFx = pos;
                toggleFx (pos, down);
                if (down) flash (fx::def (pos).name, raw (fx::paramId (groupOf (sel), pos)) > 0.5f ? "ON" : "OFF", nowMs);
                return;
            case Mode::scene:
                if (! down || num >= SnapshotManager::kSlots) return;     // pads 1-8
                if (held[shift]) { processor.getSnapshots().store (num); flash ("SNAPSHOT", "STORE " + juce::String (num + 1), nowMs); }
                else             { processor.getSnapshots().recall (num); flash ("SNAPSHOT", juce::String (num + 1), nowMs); }
                return;
        }
    }

    void MikroController::releaseEverything()
    {
        endGesture();
        if (stripParam != nullptr) stripParam->endChangeGesture();
        stripParam = nullptr;
        for (int f = 0; f < 16; ++f) toggleFx (f, false);
        setFill (false);
        heldSteps.clear();
        held.reset();
    }

    void MikroController::processPending (double nowMs)
    {
        Event e;
        while (inbox.pop (e))
        {
            switch (e.kind)
            {
                case Event::button:  handleButton (e.index, e.down, nowMs); break;
                case Event::pad:     handlePad (e.index, e.down, nowMs); break;
                case Event::encoder: turnEncoder (e.value, nowMs); break;
                case Event::strip:   handleStrip (e.value); break;
            }
        }
        if (gestureParam != nullptr && ! held[encoderTouch] && nowMs - lastTurnMs > 500.0) endGesture();
    }

    // ==========================================================================
    //  Lights and screen
    // ==========================================================================
    void MikroController::buildLights (double nowMs)
    {
        const bool blinkOn = std::fmod (nowMs, 250.0) < 125.0;
        const auto mode = getMode();
        const int sel = selectedPad.load();
        const int view = juce::jlimit (0, kNumScenes - 1, juce::roundToInt (sceneViewRaw->load()));
        lights.clear();

        // ---- Buttons: dim = available, bright = on / held.
        auto btn = [this] (int b, bool on) { lights.setButton (b, on ? levelBright : levelDim); };
        btn (padMode, mode == Mode::pads);   btn (keyboard, mode == Mode::keyboard); btn (step, mode == Mode::steps);
        btn (perform, mode == Mode::perform); btn (scene, mode == Mode::scene);
        btn (plugin, page == Page::pad && mode != Mode::perform);
        btn (swing, page == Page::seq && mode != Mode::perform);
        btn (volume, page == Page::mix && mode != Mode::perform);
        btn (group, view == 1);
        btn (left, held[left] || (mode == Mode::steps && stepPage == 0));
        btn (right, held[right] || (mode == Mode::steps && stepPage == 1));
        btn (fixedVel, fixedVelOn.load());
        btn (noteRepeat, held[noteRepeat]);
        btn (tap, fillHeld);
        for (int b : { (int) shift, (int) mikro::select, (int) duplicate, (int) erase }) btn (b, held[(size_t) b]);
        btn (mute, held[mute] || raw (params::padId (sel, params::padMute)) > 0.5f);
        btn (solo, held[solo] || raw (params::padId (sel, params::padSolo)) > 0.5f);
        if (processor.getMutator().isWaiting()) lights.setButton (variation, blinkOn ? levelBright : levelOff);
        else                                     btn (variation, false);
        switch (processor.getResampler().getState())
        {
            case Resampler::State::armed:     lights.setButton (sampling, blinkOn ? levelBright : levelOff); break;
            case Resampler::State::recording:
            case Resampler::State::saving:    lights.setButton (sampling, levelBright); break;
            case Resampler::State::idle:      btn (sampling, false); break;
        }
        btn (browse, browsing.load());
        lights.setButton (play, bridge.transport.isPlaying.load (std::memory_order_relaxed) ? levelBright : levelDim);
        lights.setButton (rec, bridge.transport.isRecording.load (std::memory_order_relaxed) || held[rec] ? levelBright : levelDim);
        btn (stop, held[stop]);

        // ---- Pads.
        for (int i = 0; i < 16; ++i)               // Mikro pad index (reading order: 0 = top left)
        {
            const int pos = i;                         // FX grid (reading order, as on screen)
            const int num = padNumberOf (i);           // pads, slices, snapshots, steps (0 = pad 1, bottom left)
            int colour = off, level = 0;
            if (browsing.load())
            {
                // Pads 1-4: what the browser shows.
                const int f = (int) processor.getPresets().getFilter();          // 1 kits, 2 pads, 3 sequences, 4 favourites
                if (num < 4) { colour = num == 3 ? yellow : white; level = f == num + 1 ? 3 : 1; }
                lights.setPad (i, colour, level);
                continue;
            }
            switch (mode)
            {
                case Mode::pads:
                {
                    const int pad = view * kPadsPerScene + num;
                    const bool muted   = raw (params::padId (pad, params::padMute)) > 0.5f;
                    const bool queued  = bridge.padQueued[(size_t) pad].load (std::memory_order_relaxed);
                    const bool running = bridge.laneStep[(size_t) pad].load (std::memory_order_relaxed) >= 0
                                      || bridge.sliceStep[(size_t) pad].load (std::memory_order_relaxed) >= 0;
                    const float act    = bridge.padActivity[(size_t) pad].load (std::memory_order_relaxed);
                    const bool seqPad  = params::isSeqMode (juce::roundToInt (raw (params::padId (pad, params::padMode))));
                    const bool loaded  = bridge.padLoaded[(size_t) pad].load (std::memory_order_relaxed) || (seqPad && bridge.seq.laneHasSteps (pad));
                    if (queued)            { colour = running ? green : orange; level = blinkOn ? 3 : 0; }
                    else if (muted)        { colour = red; level = act > 0.05f ? 3 : 1; }
                    else if (running)      { colour = green; level = 3; }
                    else if (act > 0.1f)   { colour = white; level = 3; }
                    else if (pad == sel)   { colour = orange; level = 3; }
                    else if (loaded)       { colour = orange; level = 1; }
                    break;
                }
                case Mode::steps:
                {
                    const int st = stepPage * 16 + num;   // step 1 = pad 1 (bottom left), step 16 top right
                    const int len = laneLength (sel);
                    const bool seqPad = params::isSeqMode (juce::roundToInt (raw (params::padId (sel, params::padMode))));
                    const int ph = seqPad ? bridge.laneStep[(size_t) sel].load() : bridge.seqStep.load();
                    const auto s = bridge.seq.get (sel, st);
                    bool isHeld = false;
                    for (auto& h : heldSteps) isHeld = isHeld || h.step == st;
                    // Steps in their slice's colour (the same as on the KEYBOARD pads); plain steps orange.
                    const int sc = s.slice > 0 ? s.slice : orange;
                    if (st >= len)         { colour = off; }
                    else if (isHeld)       { colour = s.on ? sc : white; level = blinkOn ? 3 : 0; }
                    else if (st == ph)     { colour = white; level = 3; }
                    else if (s.on)         { colour = sc; level = s.velocity >= 64 ? 3 : 2; }
                    else                   { colour = white; level = 1; }
                    break;
                }
                case Mode::keyboard:
                {
                    // Each slice in its own colour (as its steps show); the chosen one bright.
                    const bool loaded = bridge.padLoaded[(size_t) sel].load (std::memory_order_relaxed) && selectedHasSlices.load();
                    colour = loaded ? num + 1 : off;
                    level = chosenSlice == num + 1 ? 3 : 1;
                    if ((heldMask.load() >> i) & 1u) { colour = white; level = 3; }
                    break;
                }
                case Mode::perform:
                {
                    const int g = groupOf (sel);
                    const bool on = raw (fx::paramId (g, pos)) > 0.5f;
                    const bool armed = ((bridge.fxArmed[(size_t) g].load (std::memory_order_relaxed) >> pos) & 1u) != 0;
                    colour = cyan;
                    level = armed ? (blinkOn ? 3 : 0) : (on ? 3 : 1);
                    break;
                }
                case Mode::scene:
                {
                    if (num >= SnapshotManager::kSlots) break;
                    using S = SnapshotManager::SlotState;
                    const auto stt = processor.getSnapshots().getState (num);
                    colour = blue;
                    level = stt == S::empty ? 0 : stt == S::active ? 3 : (stt == S::loading || stt == S::waiting) ? (blinkOn ? 3 : 0) : 1;
                    if (stt == S::empty) colour = off;
                    break;
                }
            }
            lights.setPad (i, colour, level);
        }

        // ---- Touch strip: where the crossfader (SHIFT: macro amount) is.
        const bool macro = held[shift];
        const float x = raw (macro ? params::macroAmount : params::xfade);
        const float norm = macro ? x / 100.0f : x;
        const int led = juce::jlimit (0, kNumStripLeds - 1, juce::roundToInt (norm * (float) (kNumStripLeds - 1)));
        for (int s = 0; s < kNumStripLeds; ++s)
            lights.setStrip (s, macro ? (s <= led ? levelNormal : levelOff) : (s == led ? levelBright : levelOff));
    }

    void MikroController::buildScreen (double nowMs)
    {
        screen.clear();
        if (browsing.load())
        {
            auto& pm = processor.getPresets();
            const auto& list = pm.getList();
            using F = PresetManager::Filter;
            const auto f = pm.getFilter();
            juce::String head = f == F::kits ? "KITS" : f == F::pads ? "PAD PRESETS" : f == F::sequences ? "SEQUENCES" : f == F::favourites ? "FAVOURITES" : "PRESETS";
            const auto count = list.isEmpty() ? juce::String ("0") : juce::String (pm.getCursor() + 1) + "/" + juce::String (list.size());
            screen.text (0, 0, head.toRawUTF8());
            screen.text (kScreenW - 6 * count.length(), 0, count.toRawUTF8());
            for (int x = 0; x < kScreenW; ++x) screen.set (x, 8, true);
            if (nowMs < flashUntilMs)
            {
                screen.text (0, 10, flashTitle.substring (0, 21).toRawUTF8());
                screen.text (0, 18, flashValue.substring (0, 21).toRawUTF8());
                return;
            }
            if (list.isEmpty()) { screen.text (0, 14, "NOTHING HERE", 1); return; }
            const auto& m = list[juce::jlimit (0, list.size() - 1, pm.getCursor())];
            screen.text (0, 11, ((m.favourite ? "*" : "") + m.name.toUpperCase()).substring (0, 21).toRawUTF8());
            juce::String info = PresetManager::typeName (m.type);
            if (m.bpm > 0.0) info << " " << juce::String (juce::roundToInt (m.bpm)) << "BPM";
            if (m.tags.size() > 0) info << " " << m.tags[0].toUpperCase();
            screen.text (0, 22, info.substring (0, 21).toRawUTF8());
            return;
        }
        const auto mode = getMode();
        juce::String head = modeName (mode);
        if (mode == Mode::pads)  head << " " << (juce::roundToInt (sceneViewRaw->load()) == 0 ? "A" : "B");
        if (mode == Mode::steps) head << (stepPage == 0 ? " 1-16" : " 17-32");
        if (mode == Mode::steps || mode == Mode::keyboard)
        {
            const int sp = selectedPad.load();
            if (mode == Mode::keyboard && ! selectedHasSlices.load()) head << " NO SLICES";
            else if (wholeOnly (sp))  head << " W-ONLY";
            else if (chosenSlice > 0) head << " S" << juce::String (chosenSlice).paddedLeft ('0', 2);
            else                      head << " WHOLE";
        }
        screen.text (0, 0, head.toRawUTF8());
        const auto pad = padLabel (selectedPad.load());
        screen.text (kScreenW - 6 * pad.length(), 0, pad.toRawUTF8());
        for (int x = 0; x < kScreenW; ++x) screen.set (x, 8, true);

        const bool flashing = nowMs < flashUntilMs;
        const auto title = flashing ? flashTitle : paramLabel();
        const auto value = flashing ? flashValue : paramValue();
        screen.text (0, 10, title.substring (0, 21).toRawUTF8());
        screen.text (0, 18, value.substring (0, 10).toRawUTF8(), 2);
    }

    void MikroController::tick (double nowMs)
    {
        if (! enabled || hub == nullptr) return;
        if (hub->getConnectionCount() != connectionSeen)
        {
            connectionSeen = hub->getConnectionCount();
            resetInput = true;
            lightsKnown = screenKnown = false;
        }
        const bool active = isActive();
        if (! active)
        {
            if (wasActive)
            {
                // Unplugged, or another instance has it: let go of everything held here
                // (the USB thread no longer calls us: the device is closed or owned elsewhere).
                releaseHeldPlays();
                releaseEverything();
                resetInput = true;
            }
            wasActive = false;
            return;
        }
        if (! wasActive) { wasActive = true; lightsKnown = screenKnown = false; }

        {
            // A10: does the selected pad have slice markers? (KEYBOARD stays dark if not)
            const auto info = processor.getSamples().getInfo (selectedPad.load());
            selectedHasSlices = info.buffer != nullptr && ! info.slices.isEmpty();
        }
        buildLights (nowMs);
        if (! lightsKnown || lights.state != sentLights || nowMs - lastLightsMs > 2000.0)
        {
            hub->send (this, lights.report());
            sentLights = lights.state;
            lightsKnown = true;
            lastLightsMs = nowMs;
        }
        if (nowMs - lastScreenMs >= 60.0)
        {
            buildScreen (nowMs);
            if (! screenKnown || screen.raw() != sentScreen || nowMs - lastScreenMs > 2000.0)
            {
                for (auto& r : screen.reports()) hub->send (this, r);
                sentScreen = screen.raw();
                screenKnown = true;
                lastScreenMs = nowMs;
            }
        }
    }
}
