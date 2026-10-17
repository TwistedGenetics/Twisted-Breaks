#include "Core/PushController.h"
#include "Core/Parameters.h"
#include "Core/FxDefs.h"
#include "PluginProcessor.h"
#include <mutex>

namespace tb
{
    namespace
    {
        // Modifier keys, as bits (the MIDI thread's view: a pad pressed with one of these doesn't play).
        uint32_t modBit (int cc) noexcept
        {
            switch (cc)
            {
                case push::shift:        return 1u << 0;
                case push::select:       return 1u << 1;
                case push::mute:         return 1u << 2;
                case push::solo:         return 1u << 3;
                case push::duplicate:    return 1u << 4;
                case push::deleteButton: return 1u << 5;
                default:                 return 0u;
            }
        }

        // The scene column, top to bottom.
        constexpr int kSceneMutate   = push::sceneTop;        // 43
        constexpr int kSceneFill     = push::sceneTop - 1;    // 42
        constexpr int kSceneResample = push::sceneTop - 2;    // 41
        constexpr int kSceneView     = push::sceneTop - 3;    // 40
        constexpr int kSceneLayout   = push::sceneTop - 4;    // 39

        // White-LED keys this mode uses (lit dim = available, full = held / on).
        constexpr int kWhiteKeys[] = { push::left, push::right, push::up, push::down, push::pageLeft, push::pageRight,
                                       push::layout, push::fixedLength, push::duplicate, push::deleteButton, push::accent,
                                       push::repeat, push::undo, push::octaveUp, push::octaveDown, push::select, push::shift };

        const char* const kStepFieldNames[] = { "VEL", "CHANCE", "RATCHET", "PITCH", "SLICE", "MICRO", "REV", "COND" };
        constexpr int kStepFieldDivisor[]   = { 1, 1, 4, 3, 3, 1, 3, 3 };   // encoder ticks per unit

        // FX page: which effect each encoder / lower key works on, and which of its settings.
        constexpr int kFxSlots[8][2] = { { fx::lpf, 0 }, { fx::hpf, 0 }, { fx::crush, 0 }, { fx::drive, 1 },
                                         { fx::tape, 0 }, { fx::stutter, 0 }, { fx::delay, 2 }, { fx::reverb, 2 } };
    }

    //==========================================================================
    //  PushHub
    //==========================================================================
    std::shared_ptr<PushHub> PushHub::shared()
    {
        static std::mutex m;
        static std::weak_ptr<PushHub> w;
        std::lock_guard<std::mutex> g (m);
        if (auto s = w.lock()) return s;
        auto s = std::make_shared<PushHub> (true);
        w = s;
        return s;
    }

    PushHub::PushHub (bool open) : openDevices (open)
    {
        if (openDevices)
        {
            tryOpen();
            startTimer (2000);
        }
    }

    PushHub::~PushHub()
    {
        stopTimer();
        if (input != nullptr) input->stop();     // no lock held: the callback may be waiting for it
        input.reset();
        output.reset();
    }

    int PushHub::findPort (const juce::Array<juce::MidiDeviceInfo>& devices)
    {
        int best = -1, bestScore = 0;
        for (int i = 0; i < devices.size(); ++i)
        {
            const auto n = devices[i].name;
            if (! n.containsIgnoreCase ("push")) continue;
            int score = 0;
            if (n.containsIgnoreCase ("user"))                                  score = 10;   // Mac: "Ableton Push 3 User Port"
            else if (n.startsWithIgnoreCase ("MIDIIN2") || n.startsWithIgnoreCase ("MIDIOUT2")) score = 9;   // Windows
            else if (n.endsWith (":1"))                                         score = 8;    // Linux
            if (score == 0) continue;
            if (n.contains ("3")) score += 2;                                    // Push 3 before a Push 2
            if (score > bestScore) { bestScore = score; best = i; }
        }
        return best;
    }

    void PushHub::tryOpen()
    {
        if (! openDevices || testSink != nullptr) return;

        const auto ins  = juce::MidiInput::getAvailableDevices();
        const auto outs = juce::MidiOutput::getAvailableDevices();
        const int  i = findPort (ins), o = findPort (outs);

        if (input != nullptr && output != nullptr)
        {
            if (i >= 0 && o >= 0) return;            // still there
            input->stop();                            // unplugged: close, reopen when it's back
            input.reset();
            output.reset();
            portName = {};
            return;
        }
        if (i < 0 || o < 0) return;

        auto in  = juce::MidiInput::openDevice (ins[i].identifier, this);
        auto out = juce::MidiOutput::openDevice (outs[o].identifier);
        if (in == nullptr || out == nullptr) return;
        output = std::move (out);
        input  = std::move (in);
        portName = ins[i].name;
        input->start();
        connections.fetch_add (1);
    }

    void PushHub::timerCallback() { tryOpen(); }

    void PushHub::attach (PushController* c)
    {
        const juce::ScopedLock sl (lock);
        if (std::find (attached.begin(), attached.end(), c) == attached.end()) attached.push_back (c);
        if (owner == nullptr) owner = c;
    }

    void PushHub::detach (PushController* c)
    {
        const juce::ScopedLock sl (lock);
        attached.erase (std::remove (attached.begin(), attached.end(), c), attached.end());
        if (owner == c) owner = attached.empty() ? nullptr : attached.back();
    }

    void PushHub::claim (PushController* c)
    {
        const juce::ScopedLock sl (lock);
        if (std::find (attached.begin(), attached.end(), c) != attached.end()) owner = c;
    }

    bool PushHub::isOwner (const PushController* c) const
    {
        const juce::ScopedLock sl (lock);
        return c != nullptr && owner == c;
    }

    void PushHub::send (const PushController* from, const juce::MidiMessage& m)
    {
        if (! isOwner (from)) return;
        if (testSink != nullptr)      testSink->send (m);
        else if (output != nullptr)   output->sendMessageNow (m);
    }

    bool PushHub::isConnected() const { return testSink != nullptr || (input != nullptr && output != nullptr); }
    juce::String PushHub::getPortName() const { return testSink != nullptr ? juce::String ("Test Push") : portName; }

    void PushHub::dispatch (const juce::MidiMessage& m)
    {
        const juce::ScopedLock sl (lock);
        if (owner != nullptr) owner->handleIncoming (m);
    }

    void PushHub::setSinkForTesting (Sink* s)
    {
        testSink = s;
        connections.fetch_add (1);
    }

    //==========================================================================
    //  PushController
    //==========================================================================
    const juce::Identifier PushController::pushId { "PUSH" };

    PushController::PushController (TwistedBreaksProcessor& p)
        : processor (p), bridge (p.getBridge()), apvts (p.getParameters())
    {
        for (auto& v : paletteRgb) v.store (-1);
        heldPlay.fill (-1);
        sentPad.fill (-1);
        sentButton.fill (-1);
        for (int c = 0; c < push::kNumColours; ++c) colourIndex[(size_t) c] = push::fallbackIndex (c);
        sceneViewRaw = apvts.getRawParameterValue (params::sceneView);
        for (int pad = 0; pad < kNumPads; ++pad)
        {
            padRaw[(size_t) pad] = { apvts.getRawParameterValue (params::padId (pad, params::padMute)),
                                     apvts.getRawParameterValue (params::padId (pad, params::padMode)),
                                     apvts.getRawParameterValue (params::padId (pad, params::padSeqLen)),
                                     apvts.getRawParameterValue (params::padId (pad, params::padGroup)),
                                     apvts.getRawParameterValue (params::padId (pad, params::padSolo)) };
        }
        seqLengthRaw = apvts.getRawParameterValue (params::seqLength);
    }

    PushController::~PushController()
    {
        stopTimer();
        if (hub != nullptr && isActive()) allOff();
        detachHub();
    }

    // ---- Settings ------------------------------------------------------------------
    void PushController::attachHub()
    {
        if (hub == nullptr) hub = PushHub::shared();
        hub->attach (this);
        hub->claim (this);
    }

    void PushController::detachHub()
    {
        if (hub == nullptr) return;
        hub->detach (this);          // after this the MIDI thread never calls us again
        releaseHeldPlays();
        hub.reset();
    }

    void PushController::setEnabled (bool on)
    {
        if (on == enabled) return;
        enabled = on;
        if (on)
        {
            attachHub();
            needRedraw = true;
            if (! testing) startTimerHz (50);
        }
        else
        {
            stopTimer();
            if (hub != nullptr && isConnected() && isOwner() && userMode.load()) allOff();
            releaseEverything();
            detachHub();
            userMode = false;
            wasActive = false;
        }
        if (onSettingsChanged) onSettingsChanged();
    }

    void PushController::claim()
    {
        if (hub != nullptr && ! hub->isOwner (this))
        {
            hub->claim (this);
            needRedraw = true;
        }
    }

    bool PushController::isConnected() const { return hub != nullptr && hub->isConnected(); }
    bool PushController::isOwner() const     { return hub != nullptr && hub->isOwner (this); }
    juce::String PushController::getPortName() const { return hub != nullptr ? hub->getPortName() : juce::String(); }

    juce::ValueTree PushController::toValueTree() const
    {
        juce::ValueTree t (pushId);
        t.setProperty ("enabled", enabled, nullptr);
        t.setProperty ("layout", layoutAtomic.load(), nullptr);
        return t;
    }

    void PushController::fromValueTree (const juce::ValueTree& t)
    {
        if (! t.isValid()) return;
        layoutAtomic = juce::jlimit (0, 1, (int) t.getProperty ("layout", 0));
        setEnabled ((bool) t.getProperty ("enabled", false));
    }

    void PushController::useHubForTesting (std::shared_ptr<PushHub> h)
    {
        testing = true;
        stopTimer();
        if (hub != nullptr) detachHub();
        hub = std::move (h);
        enabled = true;
        attachHub();
        needRedraw = true;
    }

    void PushController::timerCallback()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        processPending (now);
        tick (now);
    }

    // ---- Selection -------------------------------------------------------------------
    void PushController::setSelectedPad (int pad)
    {
        pad = juce::jlimit (0, kNumPads - 1, pad);
        if (pad != selectedPad) { heldSteps.clear(); encoderAcc.fill (0); }   // held steps belonged to the old pad
        selectedPad = pad;
    }

    void PushController::selectPad (int pad, bool followScene)
    {
        pad = ((pad % kNumPads) + kNumPads) % kNumPads;
        if (pad != selectedPad) heldSteps.clear();
        selectedPad = pad;
        ++selectionVersion;
        processor.getMutator().setSelectedPad (pad);
        processor.getResampler().setSelectedPad (pad);
        if (followScene && sceneOf (pad) != juce::roundToInt (sceneViewRaw->load()))
            setParamPlain (params::sceneView, (float) sceneOf (pad));
    }

    juce::String PushController::getLastInput() const
    {
        const int v = lastInput.load();
        if (v < 0) return "-";
        const int st = (v >> 16) & 0xf0, d1 = (v >> 8) & 0x7f, d2 = v & 0x7f;
        switch (st)
        {
            case 0x90: return "NOTE " + juce::String (d1) + " = " + juce::String (d2);
            case 0x80: return "NOTE " + juce::String (d1) + " OFF";
            case 0xb0: return "CC " + juce::String (d1) + " = " + juce::String (d2);
            case 0xa0: return "PRESS " + juce::String (d1) + " = " + juce::String (d2);
            case 0xd0: return "PRESS " + juce::String (d1);
            case 0xe0: return "STRIP " + juce::String (d1 | (d2 << 7));
            default:   return juce::String::toHexString (v);
        }
    }

    // ---- Grid --------------------------------------------------------------------------
    int PushController::padAt (int row, int col) const noexcept
    {
        if (row < 0 || row > 7 || col < 0 || col > 7) return -1;
        if (row >= 4)
            return (col / 4) * kPadsPerScene + (row - 4) * 4 + (col % 4);    // A left, B right, 01 bottom left
        if (layoutAtomic.load() == 1)
        {
            const int view = juce::jlimit (0, kNumScenes - 1, juce::roundToInt (sceneViewRaw->load()));
            return view * kPadsPerScene + row * 4 + col / 2;                 // big pads, 2 wide, 01 bottom left
        }
        return -1;                                                              // a step
    }

    // ==========================================================================
    //  MIDI thread
    // ==========================================================================
    void PushController::enterUserMode()
    {
        if (! userMode.exchange (true)) needRedraw = true;
    }

    void PushController::leaveUserMode()
    {
        if (userMode.exchange (false))
        {
            releaseHeldPlays();
            midiMods = 0;
            needRelease = true;
        }
    }

    void PushController::releaseHeldPlays()
    {
        for (auto& pad : heldPlay)
            if (pad >= 0)
            {
                bridge.controllerPadEvents.push ({ pad, 0.0f, false });
                bridge.padPressure[(size_t) pad].store (0.0f);
                pad = -1;
            }
        lastPressedPad = -1;
    }

    void PushController::handleSysex (const juce::MidiMessage& m)
    {
        const auto* d = m.getSysExData();
        const int n = m.getSysExDataSize();
        uint8_t dev, model, cmd;
        if (! push::parseSysexHeader (d, n, dev, model, cmd)) return;

        if (cmd == push::kCmdSetMidiMode && n >= 7)
        {
            // Push echoes a mode change on both ports: this is how we know.
            sawModeSysex = true;
            sysexDev = dev;
            sysexModel = model;
            if (d[6] == 1 || d[6] == 2) enterUserMode();
            else                        leaveUserMode();
            return;
        }
        int index; juce::uint8 r, g, b;
        if (push::parsePaletteReply (d, n, index, r, g, b) && index >= 0 && index < 128)
        {
            sysexDev = dev;
            sysexModel = model;
            paletteRgb[(size_t) index].store ((r << 16) | (g << 8) | b);
            paletteReplies.fetch_add (1);
        }
    }

    void PushController::handleIncoming (const juce::MidiMessage& m)
    {
        if (m.isSysEx()) { handleSysex (m); return; }
        const auto* d = m.getRawData();
        const int n = m.getRawDataSize();
        if (n < 1 || d[0] >= 0xf0) return;                         // clock and other real-time messages
        const int status = d[0] & 0xf0;
        const int d1 = n > 1 ? (d[1] & 0x7f) : 0, d2 = n > 2 ? (d[2] & 0x7f) : 0;
        lastInput.store ((d[0] << 16) | (d1 << 8) | d2);

        if (status == 0xb0 && d1 == push::user)
        {
            // The USER key goes to both ports in both modes. Only if Push never
            // echoes the mode message do we follow the key itself.
            if (d2 > 0 && ! sawModeSysex.load())
            {
                if (userMode.load()) leaveUserMode();
                else                 enterUserMode();
            }
            return;
        }
        if (! userMode.load()) enterUserMode();       // Push only talks on the User port in User mode

        switch (status)
        {
            case 0xb0:
                if (push::isEncoderCc (d1))          { encoderTicks[(size_t) (d1 - push::kFirstEncoderCc)].fetch_add (push::relativeDelta (d2)); return; }
                if (d1 == push::kMasterEncoderCc)    { encoderTicks[8].fetch_add (push::relativeDelta (d2)); return; }
                if (d1 == push::jogWheel)            { jogTicks.fetch_add (push::relativeDelta (d2)); return; }
                if (const uint32_t bit = modBit (d1))
                {
                    if (d2 > 0) midiMods.fetch_or (bit);
                    else        midiMods.fetch_and (~bit);
                }
                break;

            case 0xe0:
                break;          // touch strip: in order with its touch note

            case 0xa0:
            {
                const int pad = heldPlay[(size_t) d1];
                if (pad >= 0) bridge.padPressure[(size_t) pad].store ((float) d2 / 127.0f);
                return;
            }

            case 0xd0:
                if (lastPressedPad >= 0) bridge.padPressure[(size_t) lastPressedPad].store ((float) d1 / 127.0f);
                return;

            case 0x90:
            case 0x80:
            {
                const bool on = status == 0x90 && d2 > 0;
                int row, col;
                if (push::noteToCell (d1, row, col))
                {
                    if (on)
                    {
                        const int pad = padAt (row, col);
                        constexpr uint32_t noPlay = 0x3f;            // any modifier: select / mute / ... instead
                        if (pad >= 0 && (midiMods.load() & noPlay) == 0)
                        {
                            // Channel pressure follows the newest pad: the previous one lets go of it.
                            if (lastPressedPad >= 0 && lastPressedPad != pad) bridge.padPressure[(size_t) lastPressedPad].store (0.0f);
                            if (heldPlay[(size_t) d1] >= 0) bridge.controllerPadEvents.push ({ heldPlay[(size_t) d1], 0.0f, false });
                            bridge.padPressure[(size_t) pad].store (0.0f);
                            bridge.controllerPadEvents.push ({ pad, juce::jlimit (1, 127, d2) / 127.0f, true });
                            heldPlay[(size_t) d1] = pad;
                            lastPressedPad = pad;
                        }
                    }
                    else if (heldPlay[(size_t) d1] >= 0)
                    {
                        const int pad = heldPlay[(size_t) d1];
                        heldPlay[(size_t) d1] = -1;
                        bridge.controllerPadEvents.push ({ pad, 0.0f, false });
                        bridge.padPressure[(size_t) pad].store (0.0f);
                        if (lastPressedPad == pad) lastPressedPad = -1;
                    }
                }
                break;
            }

            default:
                return;
        }
        inbox.push ({ (uint8_t) d[0], (uint8_t) d1, (uint8_t) d2 });
    }

    // ==========================================================================
    //  Message thread
    // ==========================================================================
    juce::RangedAudioParameter* PushController::param (const juce::String& id) const
    {
        return apvts.getParameter (id);
    }

    float PushController::raw (const juce::String& id) const
    {
        if (auto* v = apvts.getRawParameterValue (id)) return v->load();
        return 0.0f;
    }

    void PushController::setParamPlain (const juce::String& id, float plain)
    {
        if (auto* p = param (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
            p->endChangeGesture();
        }
    }

    int PushController::groupOf (int pad) const
    {
        return juce::jlimit (0, kNumGroups - 1, juce::roundToInt (padRaw[(size_t) pad].group->load()));
    }

    int PushController::laneLength (int pad) const
    {
        const int global = juce::jlimit (1, SequencerData::kSteps, juce::roundToInt (seqLengthRaw->load()));
        const auto& r = padRaw[(size_t) pad];
        const int own = juce::roundToInt (r.seqLen->load());
        return params::isSeqMode (juce::roundToInt (r.mode->load())) && own > 0 ? juce::jmin (own, SequencerData::kSteps) : global;
    }

    int PushController::fxForSlot (int slot) noexcept { return kFxSlots[juce::jlimit (0, 7, slot)][0]; }

    const char* PushController::pageName (Page p) noexcept
    {
        switch (p)
        {
            case Page::pad: return "PAD";
            case Page::fx:  return "FX";
            case Page::seq: return "SEQ";
            case Page::mix: return "MIX";
        }
        return "";
    }

    std::array<PushController::Target, 8> PushController::targets() const
    {
        std::array<Target, 8> t;
        if (! heldSteps.empty())
        {
            for (int i = 0; i < 8; ++i) { t[(size_t) i].isStep = true; t[(size_t) i].field = i; t[(size_t) i].label = kStepFieldNames[i]; }
            return t;
        }
        auto set = [&t] (int i, const juce::String& id, const juce::String& label) { t[(size_t) i].paramId = id; t[(size_t) i].label = label; };
        const int pad = selectedPad;
        switch (page)
        {
            case Page::pad:
            {
                const char* ids[]    = { params::padGain, params::padPan, params::padPitch, params::padStart,
                                         params::padEnd, params::padSlice, params::padCharacter, params::padPress };
                const char* labels[] = { "GAIN", "PAN", "PITCH", "START", "END", "SLICE", "CHARACTER", "PRESSURE" };
                for (int i = 0; i < 8; ++i) set (i, params::padId (pad, ids[i]), labels[i]);
                break;
            }
            case Page::fx:
            {
                const int g = groupOf (pad);
                for (int i = 0; i < 8; ++i)
                {
                    const int f = kFxSlots[i][0], k = kFxSlots[i][1];
                    set (i, fx::paramId (g, f, k), juce::String (fx::def (f).label) + " " + fx::def (f).params[k].label);
                }
                break;
            }
            case Page::seq:
                set (0, params::seqRes, "STEP");
                set (1, params::seqLength, "LEN");
                set (2, params::padId (pad, params::padSeqLen), "PAD LEN");
                set (3, params::padId (pad, params::padLaunch), "LAUNCH");
                set (4, params::mutAmount, "MUTATE");
                set (5, params::mutAuto, "AUTO");
                set (6, params::mutAt, "MUTATE AT");
                set (7, params::seqQuant, "QUANTISE");
                break;
            case Page::mix:
                for (int g = 0; g < kNumGroups; ++g)
                    set (g, params::groupId (g, params::grpLevel), "GROUP " + juce::String::charToString ((juce::juce_wchar) ('A' + g)));
                set (4, params::xfade, "XFADE");
                set (5, params::xfadeCurve, "CURVE");
                set (6, params::macroAmount, "MACRO");
                set (7, params::masterGain, "MASTER");
                break;
        }
        return t;
    }

    juce::String PushController::encoderLabel (int i) const
    {
        return juce::isPositiveAndBelow (i, 8) ? targets()[(size_t) i].label : juce::String();
    }

    juce::String PushController::encoderValue (int i) const
    {
        if (! juce::isPositiveAndBelow (i, 8)) return {};
        const auto t = targets()[(size_t) i];
        if (t.isStep)
        {
            if (heldSteps.empty()) return {};
            const auto st = bridge.seq.get (selectedPad, heldSteps.front().step);
            switch (t.field)
            {
                case 0: return juce::String (st.velocity);
                case 1: return juce::String (st.probability) + " %";
                case 2: return "x" + juce::String (st.ratchet);
                case 3: return (st.pitch > 0 ? "+" : "") + juce::String (st.pitch) + " st";
                case 4: return st.slice == 0 ? juce::String ("PAD") : juce::String (st.slice);
                case 5: return (st.micro > 0 ? "+" : "") + juce::String (st.micro);
                case 6: return st.rev ? "ON" : "OFF";
                case 7: return kSeqCondNames[juce::jlimit (0, kNumSeqConds - 1, st.cond)];
                default: return {};
            }
        }
        if (auto* p = param (t.paramId)) return p->getCurrentValueAsText();
        return {};
    }

    int PushController::getActiveEncoder() const noexcept
    {
        for (int i = 0; i < 8; ++i) if (encoderTouched[(size_t) i]) return i;
        return juce::Time::getMillisecondCounterHiRes() - activeEncoderMs < 1500.0 ? activeEncoder : -1;
    }

    void PushController::endGesture (int i)
    {
        if (auto* p = gestureParam[(size_t) i]) p->endChangeGesture();
        gestureParam[(size_t) i] = nullptr;
    }

    void PushController::touchEncoder (int i, bool touched, double nowMs)
    {
        encoderTouched[(size_t) i] = touched;
        if (touched) { activeEncoder = i; activeEncoderMs = nowMs; }
        else
        {
            endGesture (i);
            activeEncoderMs = nowMs;
        }
    }

    void PushController::turnEncoder (int i, int ticks, double nowMs)
    {
        const bool shift = held[push::shift];
        activeEncoder = i;
        activeEncoderMs = nowMs;
        lastTurnMs[(size_t) i] = nowMs;

        Target t;
        if (i == 8) t.paramId = params::masterGain;
        else        t = targets()[(size_t) i];

        if (t.isStep)
        {
            auto& acc = encoderAcc[(size_t) i];
            acc += ticks;
            const int div = kStepFieldDivisor[t.field];
            const int units = acc / div;
            acc -= units * div;
            if (units != 0) editStepField (t.field, units);
            return;
        }

        auto* p = param (t.paramId);
        if (p == nullptr) return;
        if (gestureParam[(size_t) i] != p)
        {
            endGesture (i);
            p->beginChangeGesture();
            gestureParam[(size_t) i] = p;
        }

        if (p->isDiscrete())
        {
            auto& acc = encoderAcc[(size_t) i];
            acc += ticks;
            const int div = shift ? 6 : 3;
            const int units = acc / div;
            acc -= units * div;
            if (units == 0) return;
            const auto range = p->getNormalisableRange();
            const float plain = juce::jlimit (range.start, range.end, (float) juce::roundToInt (p->convertFrom0to1 (p->getValue())) + (float) units);
            p->setValueNotifyingHost (p->convertTo0to1 (plain));
        }
        else
        {
            const float step = shift ? 0.001f : 1.0f / 150.0f;
            p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, p->getValue() + step * (float) ticks));
        }
    }

    void PushController::editStepField (int field, int units)
    {
        auto& seq = bridge.seq;
        for (auto& h : heldSteps)
        {
            auto st = seq.get (selectedPad, h.step);
            st.on = true;
            switch (field)
            {
                case 0: st.velocity    = juce::jlimit (1, 127, st.velocity + units); break;
                case 1: st.probability = juce::jlimit (0, 100, st.probability + units); break;
                case 2: st.ratchet     = juce::jlimit (1, 4, st.ratchet + units); break;
                case 3: st.pitch       = juce::jlimit (-24, 24, st.pitch + units); break;
                case 4: st.slice       = juce::jlimit (0, 16, st.slice + units); break;
                case 5: st.micro       = juce::jlimit (-64, 63, st.micro + units); break;
                case 6: st.rev         = units > 0; break;
                case 7: st.cond        = juce::jlimit (0, kNumSeqConds - 1, st.cond + units); break;
                default: break;
            }
            seq.set (selectedPad, h.step, st);
            h.edited = true;
        }
        processor.markStateChanged();
    }

    void PushController::pressStep (int step, int /*velocity*/)
    {
        auto& seq = bridge.seq;
        if (held[push::deleteButton])
        {
            seq.set (selectedPad, step, SeqStep {});
            processor.markStateChanged();
            return;
        }
        if (! heldSteps.empty())
        {
            // Hold one step, tap another: copy the first across the range.
            auto& first = heldSteps.front();
            const auto src = seq.get (selectedPad, first.step);
            for (int k = juce::jmin (first.step, step); k <= juce::jmax (first.step, step); ++k)
                seq.set (selectedPad, k, src);
            first.edited = true;
            heldSteps.push_back ({ step, false, true });
            processor.markStateChanged();
            return;
        }
        auto st = seq.get (selectedPad, step);
        const bool created = ! st.on;
        if (created)
        {
            st = SeqStep {};
            st.on = true;
            seq.set (selectedPad, step, st);
            processor.markStateChanged();
        }
        heldSteps.push_back ({ step, created, false });
        encoderAcc.fill (0);
    }

    void PushController::releaseStep (int step)
    {
        auto it = std::find_if (heldSteps.begin(), heldSteps.end(), [step] (const HeldStep& h) { return h.step == step; });
        if (it == heldSteps.end()) return;
        if (! it->created && ! it->edited)
        {
            auto st = bridge.seq.get (selectedPad, step);
            st.on = false;
            bridge.seq.set (selectedPad, step, st);
            processor.markStateChanged();
        }
        heldSteps.erase (it);
        if (heldSteps.empty()) { encoderAcc.fill (0); }
    }

    void PushController::handleCell (int row, int col, int velocity, bool on, double)
    {
        const int pad = padAt (row, col);
        if (pad >= 0)
        {
            if (! on) return;
            auto toggle = [this, pad] (const char* name)
            {
                const auto id = params::padId (pad, name);
                setParamPlain (id, raw (id) > 0.5f ? 0.0f : 1.0f);
            };
            if      (held[push::mute])         toggle (params::padMute);
            else if (held[push::solo])         toggle (params::padSolo);
            else if (held[push::duplicate])
            {
                if (pad != selectedPad)
                {
                    for (int s = 0; s < SequencerData::kSteps; ++s)
                        bridge.seq.setPacked (selectedPad, s, bridge.seq.getPacked (pad, s));
                    bridge.seq.bumpVersion();
                    processor.markStateChanged();
                }
            }
            else if (held[push::deleteButton])
            {
                bridge.seq.clearLane (pad);
                processor.markStateChanged();
            }
            else selectPad (pad);
            return;
        }
        const int step = (3 - row) * 8 + col;
        if (on) pressStep (step, velocity);
        else    releaseStep (step);
    }

    void PushController::setPage (Page p)
    {
        for (int i = 0; i < 8; ++i) endGesture (i);
        encoderAcc.fill (0);
        page = p;
    }

    void PushController::cycleLength (bool padOwn)
    {
        if (padOwn)
        {
            const int opts[] = { 0, 4, 8, 16, 32 };
            const auto id = params::padId (selectedPad, params::padSeqLen);
            const int now = juce::roundToInt (raw (id));
            int next = 0;
            for (int o : opts) if (o > now) { next = o; break; }
            setParamPlain (id, (float) next);
        }
        else
        {
            const int now = juce::roundToInt (seqLengthRaw->load());
            setParamPlain (params::seqLength, (float) (now < 8 ? 8 : now < 16 ? 16 : now < 32 ? 32 : 8));
        }
    }

    void PushController::setFill (bool on)
    {
        if (on == fillHeld) return;
        fillHeld = on;
        setParamPlain (params::seqFill, on ? 1.0f : 0.0f);
    }

    void PushController::toggleFx (int slot, bool pressed)
    {
        if (! juce::isPositiveAndBelow (slot, 8)) return;
        auto& heldId = fxHeld[(size_t) slot];
        if (! pressed)
        {
            // Release exactly what the press switched on (even after a page or pad change).
            if (heldId.isNotEmpty()) setParamPlain (heldId, 0.0f);
            heldId = {};
            return;
        }
        if (page != Page::fx) return;
        const int f = fxForSlot (slot);
        const auto id = fx::paramId (groupOf (selectedPad), f);
        if (processor.isFxLocked (f))
            setParamPlain (id, raw (id) > 0.5f ? 0.0f : 1.0f);
        else if (raw (id) < 0.5f)
        {
            setParamPlain (id, 1.0f);                             // momentary: on while held
            heldId = id;
        }
    }

    void PushController::handleButton (int cc, bool pressed, double)
    {
        const bool was = held[(size_t) cc];
        held[(size_t) cc] = pressed;

        if (! pressed)
        {
            if (cc == push::repeat || cc == kSceneFill) setFill (held[push::repeat] || held[kSceneFill]);
            if (push::isLowerRow (cc) && was) toggleFx (cc - push::lowerRow1, false);
            return;
        }

        const bool shift = held[push::shift];
        const int view = juce::roundToInt (sceneViewRaw->load());
        auto showScene = [this] (int scene)
        {
            setParamPlain (params::sceneView, (float) scene);
            selectPad (scene * kPadsPerScene + selectedPad % kPadsPerScene, false);
        };

        if (push::isUpperRow (cc))
        {
            const int i = cc - push::upperRow1;
            if (i < 4) setPage ((Page) i);
            return;
        }
        if (push::isLowerRow (cc)) { toggleFx (cc - push::lowerRow1, true); return; }

        switch (cc)
        {
            case push::left:       selectPad (selectedPad - 1); break;
            case push::right:      selectPad (selectedPad + 1); break;
            case push::up:
            case push::octaveUp:   showScene (0); break;
            case push::down:
            case push::octaveDown: showScene (1); break;
            case kSceneView:       showScene (1 - view); break;
            case push::pageLeft:   setPage ((Page) (((int) page + 3) % 4)); break;
            case push::pageRight:  setPage ((Page) (((int) page + 1) % 4)); break;
            case push::layout:
            case kSceneLayout:
                heldSteps.clear();
                layoutAtomic = 1 - layoutAtomic.load();
                if (onSettingsChanged) onSettingsChanged();
                break;
            case push::fixedLength: cycleLength (shift); break;
            case push::accent:
            case kSceneMutate:      processor.getMutator().mutate(); break;
            case push::repeat:
            case kSceneFill:        setFill (true); break;
            case kSceneResample:
            {
                auto& rs = processor.getResampler();
                rs.setSelectedPad (selectedPad);
                if (rs.getState() == Resampler::State::idle) rs.record();
                else                                         rs.cancel();
                break;
            }
            case push::undo:        if (processor.getMutator().canUndo()) processor.getMutator().undo(); break;
            default: break;
        }
    }

    void PushController::releaseEverything()
    {
        for (int i = 0; i < 9; ++i) endGesture (i);
        encoderTouched.fill (false);
        encoderAcc.fill (0);
        if (stripTouched && stripParam != nullptr) stripParam->endChangeGesture();
        stripParam = nullptr;
        stripTouched = false;
        for (int k = 0; k < 8; ++k) toggleFx (k, false);
        setFill (false);
        heldSteps.clear();
        held.reset();
    }

    void PushController::processPending (double nowMs)
    {
        if (needRedraw.exchange (false))
        {
            sentPad.fill (-1); sentButton.fill (-1);
            // Per-pad pressure (poly aftertouch) while we have Push.
            if (hub != nullptr && userMode.load())
                hub->send (this, push::sysex ((uint8_t) sysexDev.load(), (uint8_t) sysexModel.load(), { 0x1E, 0x01 }));
        }

        RawIn r;
        while (inbox.pop (r))
        {
            const int status = r.b0 & 0xf0;
            if (status == 0x90 || status == 0x80)
            {
                const bool on = status == 0x90 && r.b2 > 0;
                int row, col;
                if (r.b1 < 8)                          touchEncoder (r.b1, on, nowMs);
                else if (r.b1 == push::kMasterTouchNote) touchEncoder (8, on, nowMs);
                else if (r.b1 == push::kStripTouchNote)
                {
                    stripTouched = on;
                    if (on)
                    {
                        stripParam = param (params::xfade);
                        if (stripParam != nullptr) stripParam->beginChangeGesture();
                    }
                    else
                    {
                        if (stripParam != nullptr) stripParam->endChangeGesture();
                        stripParam = nullptr;
                    }
                }
                else if (push::noteToCell (r.b1, row, col)) handleCell (row, col, r.b2, on, nowMs);
            }
            else if (status == 0xb0) handleButton (r.b1, r.b2 > 0, nowMs);
            else if (status == 0xe0 && stripTouched && stripParam != nullptr)
            {
                // Touch strip = crossfader, while touched (it springs back on release: ignored).
                stripParam->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, (float) (r.b1 | (r.b2 << 7)) / 16383.0f));
            }
        }

        for (int i = 0; i < 9; ++i)
            if (const int t = encoderTicks[(size_t) i].exchange (0)) turnEncoder (i, t, nowMs);
        if (const int j = jogTicks.exchange (0)) selectPad (selectedPad + (j > 0 ? 1 : -1));

        // Left User mode: let go of everything, after anything queued before it (FILL, FX keys).
        if (needRelease.exchange (false)) releaseEverything();

        // Turned without touching (or touch not sent): end the gesture after a pause.
        for (int i = 0; i < 9; ++i)
            if (gestureParam[(size_t) i] != nullptr && ! encoderTouched[(size_t) i] && nowMs - lastTurnMs[(size_t) i] > 500.0)
                endGesture (i);
    }

    // ==========================================================================
    //  Lights
    // ==========================================================================
    int PushController::padColour (int pad, bool blinkOn) const
    {
        using namespace push;
        const auto& r = padRaw[(size_t) pad];
        const bool muted   = r.mute->load() > 0.5f;
        const bool queued  = bridge.padQueued[(size_t) pad].load (std::memory_order_relaxed);
        const bool running = bridge.laneStep[(size_t) pad].load (std::memory_order_relaxed) >= 0
                          || bridge.sliceStep[(size_t) pad].load (std::memory_order_relaxed) >= 0;
        const float act    = bridge.padActivity[(size_t) pad].load (std::memory_order_relaxed);
        const bool seqPad  = params::isSeqMode (juce::roundToInt (r.mode->load()));
        const bool loaded  = bridge.padLoaded[(size_t) pad].load (std::memory_order_relaxed)
                          || (seqPad && bridge.seq.laneHasSteps (pad));

        if (queued)          return blinkOn ? (running ? green : amber) : off;
        if (muted)           return act > 0.05f ? red : redDim;
        if (running)         return green;
        if (act > 0.1f)      return white;
        if (pad == selectedPad) return amber;
        return loaded ? amberDim : off;
    }

    int PushController::stepColour (int step, int laneLen, int playhead) const
    {
        using namespace push;
        if (step >= laneLen) return off;
        for (auto& h : heldSteps) if (h.step == step) return green;
        if (step == playhead) return white;
        const auto st = bridge.seq.get (selectedPad, step);
        if (st.on) return st.velocity >= 64 ? amber : amberDim;
        return dark;
    }

    void PushController::sendPad (int note, int value, bool force)
    {
        auto& sent = sentPad[(size_t) (note - push::kFirstPadNote)];
        if (! force && sent == value) return;
        sent = value;
        hub->send (this, juce::MidiMessage::noteOn (1, note, (juce::uint8) juce::jlimit (0, 127, value)));
    }

    void PushController::sendButton (int cc, int value, bool force)
    {
        auto& sent = sentButton[(size_t) cc];
        if (! force && sent == value) return;
        sent = value;
        hub->send (this, juce::MidiMessage::controllerEvent (1, cc, juce::jlimit (0, 127, value)));
    }

    void PushController::allOff()
    {
        if (hub == nullptr) return;
        for (int k = 0; k < 64; ++k) sendPad (push::kFirstPadNote + k, 0, true);
        for (int k = 0; k < 8; ++k) { sendButton (push::upperRow1 + k, 0, true); sendButton (push::lowerRow1 + k, 0, true); }
        for (int cc = push::sceneBottom; cc <= push::sceneTop; ++cc) sendButton (cc, 0, true);
        for (int cc : kWhiteKeys) sendButton (cc, 0, true);
        sendButton (push::mute, 0, true);
        sendButton (push::solo, 0, true);
    }

    void PushController::requestPalette (double nowMs)
    {
        const int dev = sysexDev.load(), model = sysexModel.load();
        if ((dev != paletteDev || model != paletteModel) && paletteReplies.load() == 0)
        {
            paletteDev = dev; paletteModel = model;     // Push answered with another header: ask again with it
            paletteNext = 0;
        }
        if (paletteNext >= 128 || nowMs - paletteRequestedMs < 20.0) return;
        paletteRequestedMs = nowMs;
        // Sysex goes through in Live mode too, so this can happen before User mode.
        for (int k = 0; k < 16 && paletteNext < 128; ++k, ++paletteNext)
        {
            const auto m = push::sysex ((uint8_t) dev, (uint8_t) model, { push::kCmdGetPalette, (uint8_t) paletteNext });
            hub->send (this, m);
        }
    }

    void PushController::tick (double nowMs)
    {
        using namespace push;
        if (! enabled || hub == nullptr) return;

        if (hub->getConnectionCount() != connectionSeen)
        {
            connectionSeen = hub->getConnectionCount();
            paletteNext = 0;
            paletteDev = sysexDev.load(); paletteModel = sysexModel.load();
            sentPad.fill (-1); sentButton.fill (-1);
        }
        if (isConnected() && isOwner()) requestPalette (nowMs);

        const auto replies = paletteReplies.load();
        if (replies != paletteSeen)
        {
            paletteSeen = replies;
            std::array<int, 128> pal {};
            for (int i = 0; i < 128; ++i) pal[(size_t) i] = paletteRgb[(size_t) i].load();
            colourIndex = mapColours (pal);
            sentPad.fill (-1); sentButton.fill (-1);
        }

        const bool active = isActive();
        if (! active)
        {
            if (wasActive && ! isOwner())
            {
                // Another instance took Push: let go of whatever was held here.
                releaseHeldPlays();
                releaseEverything();
            }
            wasActive = false;
            return;
        }
        if (! wasActive || nowMs - lastFullRefreshMs > 5000.0)
        {
            wasActive = true;
            lastFullRefreshMs = nowMs;
            sentPad.fill (-1); sentButton.fill (-1);
        }

        const bool blinkOn = std::fmod (nowMs, 250.0) < 125.0;
        auto idx = [this] (int c) { return colourIndex[(size_t) c]; };

        // ---- Grid.
        const int s = selectedPad;
        const int len = laneLength (s);
        const bool seqPad = params::isSeqMode (juce::roundToInt (padRaw[(size_t) s].mode->load()));
        const int playhead = seqPad ? bridge.laneStep[(size_t) s].load() : bridge.seqStep.load();
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
            {
                const int pad = padAt (row, col);
                const int c = pad >= 0 ? padColour (pad, blinkOn) : stepColour ((3 - row) * 8 + col, len, playhead);
                sendPad (padNote (row, col), idx (c), false);
            }

        // ---- Keys over / under the display.
        const bool steps = ! heldSteps.empty();
        for (int i = 0; i < 8; ++i)
            sendButton (upperRow1 + i, i < 4 ? idx (! steps && (int) page == i ? white : dark) : 0, false);
        const int g = groupOf (s);
        for (int i = 0; i < 8; ++i)
        {
            int c = off;
            if (page == Page::fx && ! steps)
                c = raw (fx::paramId (g, fxForSlot (i))) > 0.5f ? amber : amberDim;
            sendButton (lowerRow1 + i, idx (c), false);
        }

        // ---- Scene column.
        auto& mut = processor.getMutator();
        sendButton (kSceneMutate, idx (mut.isWaiting() ? (blinkOn ? amber : off) : amberDim), false);
        sendButton (kSceneFill, idx (fillHeld ? green : greenDim), false);
        int rc = redDim;
        switch (processor.getResampler().getState())
        {
            case Resampler::State::armed:     rc = blinkOn ? red : off; break;
            case Resampler::State::recording: rc = red; break;
            case Resampler::State::saving:    rc = amber; break;
            case Resampler::State::idle:      break;
        }
        sendButton (kSceneResample, idx (rc), false);
        sendButton (kSceneView, idx (juce::roundToInt (sceneViewRaw->load()) == 0 ? amber : blue), false);
        sendButton (kSceneLayout, idx (layoutAtomic.load() == 0 ? amberDim : amber), false);
        for (int cc = sceneBottom; cc < kSceneLayout; ++cc) sendButton (cc, 0, false);

        // ---- Mute / solo show the selected pad; white keys dim, full while held.
        sendButton (push::mute, idx (padRaw[(size_t) s].mute->load() > 0.5f || held[push::mute] ? red : redDim), false);
        sendButton (push::solo, idx (padRaw[(size_t) s].solo->load() > 0.5f || held[push::solo] ? blue : dark), false);
        for (int cc : kWhiteKeys)
            sendButton (cc, held[(size_t) cc] || (cc == push::layout && layoutAtomic.load() == 1) ? kWhiteFull : kWhiteHalf, false);
    }
}
