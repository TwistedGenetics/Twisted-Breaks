#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <memory>
#include <vector>
#include "Core/EngineBridge.h"
#include "Core/MikroProtocol.h"
#include "Core/MikroUsb.h"

class TwistedBreaksProcessor;

namespace tb
{
    class MikroController;

    /*
        MikroHub
        --------
        The Maschine Mikro MK3 over USB, once per process (every Twisted
        Breaks instance shares it; the one you touched last owns it). A
        background thread keeps trying to open it every 2 s, so plugging it in
        later, or quitting NI's Maschine app (which takes the Mikro over),
        just works. Input reports go to the owner on the USB thread; output
        reports from anyone else are dropped. Never the audio thread.
    */
    class MikroHub final : private juce::Thread
    {
    public:
        static std::shared_ptr<MikroHub> shared();
        explicit MikroHub (bool openDevice);
        ~MikroHub() override;

        void attach (MikroController*);
        void detach (MikroController*);
        void claim (MikroController*);
        bool isOwner (const MikroController*) const;
        bool isConnected() const noexcept { return connected.load(); }
        juce::String getStatus() const;
        /** Bumps on every (re)connection: time to redraw. */
        uint32_t getConnectionCount() const noexcept { return connections.load(); }

        /** Message thread: an output report, if `from` owns the Mikro. */
        void send (const MikroController* from, const std::vector<uint8_t>& report);
        /** An input report for the owner (USB thread; tests call it directly). */
        void dispatch (const uint8_t* data, int size);

        // ---- Testing ---------------------------------------------------------------
        struct Sink { virtual ~Sink() = default; virtual void send (const std::vector<uint8_t>&) = 0; };
        void setSinkForTesting (Sink*);

    private:
        void run() override;

        const bool openDevice;
        mutable juce::CriticalSection lock;        // owner / attach vs the USB thread
        juce::CriticalSection deviceLock;          // open / close vs writes
        std::vector<MikroController*> attached;
        MikroController* owner = nullptr;
        MikroUsb usb;
        std::atomic<bool> connected { false }, removed { false };
        std::atomic<uint32_t> connections { 0 };
        juce::String status;
        Sink* testSink = nullptr;
        // Latest lights / screen halves waiting to go out (the hub thread writes them, so
        // the message thread never waits on USB). Only the newest of each matters.
        juce::CriticalSection outLock;
        std::array<std::vector<uint8_t>, 3> outSlots;     // lights, screen top, screen bottom
        std::array<bool, 3> outDirty {};
        int writeFailures = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MikroHub)
    };

    /*
        MikroController  (spec addition A8: Mikro MK3 mode)
        ---------------
        The Mikro as a dedicated Twisted Breaks controller:

          PAD MODE   the 16 pads of the scene on view (matrix layout, 01 top
                     left), velocity + pressure; lit like the plugin's tiles.
                     GROUP flips scene A / B.
          STEP       the selected pad's steps 1-16 (< > for 17-32): tap = on /
                     off, hold + encoder = edit (push = next field).
          KEYBOARD   the selected pad's slices 1-16 on the pads.
          PERFORM    the 16 performance FX of the selected pad's group.
          SCENE      pads 1-8 recall snapshots 1-8 (SHIFT + pad stores).
          Encoder    PLUG-IN = PAD page, SWING = SEQ page, VOLUME = MIX page
                     (PERFORM: the last effect's settings); turn = value,
                     push or SHIFT + turn = next parameter. The screen shows
                     mode, pad, parameter and value.
          Strip      crossfader (SHIFT: macro amount), LEDs show it.
          Buttons    MUTE / SOLO / DUPLICATE / ERASE / SELECT + pad, FIXED VEL,
                     VARIATION = MUTATE (SHIFT: undo), TAP = FILL (hold),
                     SAMPLING = RESAMPLE, NOTE REPEAT (hold) = rolls at 1/16
                     (SHIFT + NOTE REPEAT: 1/8, 1/16, 1/32), PLAY lit while Live
                     plays.

        Threads: pad plays, slice hits and rolls go from the USB thread into
        the engine's own lock-free queue; everything else is queued to the
        message thread (50 Hz), which sends lights and the screen only when
        they change.
    */
    class MikroController final : private juce::Timer
    {
    public:
        explicit MikroController (TwistedBreaksProcessor&);
        ~MikroController() override;

        enum class Mode { pads, steps, keyboard, perform, scene };
        enum class Page { pad, seq, mix, fx };

        void setEnabled (bool);
        bool isEnabled() const noexcept { return enabled; }
        void claim();
        bool isConnected() const;
        bool isOwner() const;
        bool isActive() const { return enabled && isConnected() && isOwner(); }
        juce::String getHubStatus() const;

        std::function<void()> onSettingsChanged;
        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static const juce::Identifier mikroId;

        Mode getMode() const noexcept { return (Mode) modeAtomic.load(); }
        Page getPage() const noexcept { return page; }
        static const char* modeName (Mode) noexcept;
        juce::String paramLabel() const;
        juce::String paramValue() const;
        int  getSelectedPad() const noexcept { return selectedPad.load(); }
        void setSelectedPad (int pad);
        uint32_t getSelectionVersion() const noexcept { return selectionVersion; }
        int  getStepPage() const noexcept { return stepPage; }

        // ---- USB thread (from the hub) ---------------------------------------------------
        void handleReport (const uint8_t* data, int size);

        // ---- Testing ---------------------------------------------------------------------
        void useHubForTesting (std::shared_ptr<MikroHub>);
        void processPending (double nowMs);
        void tick (double nowMs);
        const mikro::Lights& getLights() const noexcept { return lights; }
        const mikro::Screen& getScreen() const noexcept { return screen; }
        /** Plugin pad position (0..15, 01 top left) of a Mikro pad index (0 = bottom left), and back. */
        static int positionOf (int hidPad) noexcept { return (3 - hidPad / 4) * 4 + hidPad % 4; }
        static int hidPadOf (int position) noexcept { return positionOf (position); }   // the same swap both ways

    private:
        struct Param { juce::String id; juce::String label; };

        void timerCallback() override;
        void attachHub();
        void detachHub();
        void releaseHeldPlays();          // USB-thread data, only when the USB thread can't call us
        void releaseEverything();

        // Message thread
        void handleButton (int button, bool down, double nowMs);
        void handlePad (int position, bool down, double nowMs);
        void turnEncoder (int ticks, double nowMs);
        void pushEncoder();
        void handleStrip (int value);
        std::vector<Param> pageParams() const;
        juce::RangedAudioParameter* param (const juce::String& id) const;
        float raw (const juce::String& id) const;
        void  setParamPlain (const juce::String& id, float plain);
        void  selectPad (int pad);
        void  setMode (Mode);
        void  setPage (Page);
        void  flash (const juce::String& title, const juce::String& value, double nowMs);
        void  endGesture();
        void  setFill (bool);
        void  toggleFx (int fxId, bool down);
        int   laneLength (int pad) const;
        int   groupOf (int pad) const;
        void  editStep (int units);

        // Lights / screen
        void buildLights (double nowMs);
        void buildScreen (double nowMs);

        TwistedBreaksProcessor& processor;
        EngineBridge& bridge;
        juce::AudioProcessorValueTreeState& apvts;
        std::atomic<float>* sceneViewRaw = nullptr;
        std::atomic<float>* seqLengthRaw = nullptr;

        std::shared_ptr<MikroHub> hub;
        bool enabled = false, testing = false;

        // ---- Shared with the USB thread -------------------------------------------------------
        SpscQueue<mikro::Event, 512> inbox;
        std::atomic<int> modeAtomic { 0 };
        std::atomic<int> selectedPad { 0 };
        std::atomic<bool> fixedVelOn { false };
        std::atomic<int> rollRate { 1 };
        std::atomic<bool> needRelease { false };
        std::atomic<bool> resetInput { true };       // new owner / reconnect: forget held keys and the encoder's position
        std::atomic<uint32_t> heldMask { 0 };        // Mikro pads held (for the lights)
        // USB thread only:
        mikro::InputParser parser;
        std::array<int, 16> heldPlay {};              // Mikro pad -> engine pad playing (-1)
        std::array<bool, 16> heldRoll {};
        std::array<bool, 16> heldSlice {};            // the press was a slice (KEYBOARD): no release
        std::array<float, 16> heldVel {};
        void releaseHeld (int i);
        uint32_t usbMods = 0;                          // modifier keys, as the USB thread sees them
        bool usbRepeatHeld = false;

        // ---- Message thread -------------------------------------------------------------------
        Page page = Page::pad;
        std::array<int, 4> paramIndex {};
        int lastFx = 1;                                // fx::lpf
        int stepPage = 0;
        uint32_t selectionVersion = 0;
        std::bitset<mikro::kNumButtons> held;
        struct HeldStep { int step; bool created; bool edited; };
        std::vector<HeldStep> heldSteps;
        int stepField = 0;
        int encoderAcc = 0;
        juce::RangedAudioParameter* gestureParam = nullptr;
        double lastTurnMs = -1.0e9;
        juce::RangedAudioParameter* stripParam = nullptr;
        bool fillHeld = false;
        std::array<juce::String, 16> fxHeldId;         // momentary FX a pad switched on (its parameter, to release)
        Mode shownMode = Mode::pads;                   // what the message thread last set up for
        juce::String flashTitle, flashValue;
        double flashUntilMs = -1.0;

        mikro::Lights lights;
        mikro::Screen screen;
        std::array<uint8_t, 80> sentLights {};
        std::array<uint8_t, 512> sentScreen {};
        bool lightsKnown = false, screenKnown = false;
        double lastLightsMs = -1.0e9, lastScreenMs = -1.0e9;
        uint32_t connectionSeen = 0;
        bool wasActive = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MikroController)
    };
}
