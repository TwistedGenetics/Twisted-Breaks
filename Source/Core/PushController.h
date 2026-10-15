#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <vector>
#include "Core/EngineBridge.h"
#include "Core/PushProtocol.h"

class TwistedBreaksProcessor;

namespace tb
{
    class PushController;

    /*
        PushHub
        -------
        The Push 3 User port, once per process: every Twisted Breaks instance
        in the set shares it, and exactly one of them owns it (the one you
        last touched). Incoming MIDI goes to the owner only; light messages
        from any other instance are dropped, so instances never fight.

        Opens the port by name ("Ableton Push 3 User Port" on the Mac, Push 2
        works too), retrying every 2 s, so unplugging and replugging Push just
        works. Never touches the audio thread.
    */
    class PushHub final : public juce::MidiInputCallback,
                          private juce::Timer
    {
    public:
        /** The hub on the real device, shared by every instance (opened while anyone holds it). */
        static std::shared_ptr<PushHub> shared();

        explicit PushHub (bool openDevices);
        ~PushHub() override;

        // ---- Message thread --------------------------------------------------------
        void attach (PushController*);
        void detach (PushController*);
        void claim (PushController*);
        bool isOwner (const PushController*) const;
        /** Sends to Push if `from` owns it. */
        void send (const PushController* from, const juce::MidiMessage&);
        bool isConnected() const;
        juce::String getPortName() const;
        /** Bumps every time the port is (re)opened: time to read the palette and redraw. */
        uint32_t getConnectionCount() const noexcept { return connections.load(); }

        /** An incoming message for the owner (the MIDI thread; tests call it directly). */
        void dispatch (const juce::MidiMessage&);

        // ---- Testing -----------------------------------------------------------------
        struct Sink
        {
            virtual ~Sink() = default;
            virtual void send (const juce::MidiMessage&) = 0;
        };
        /** Messages go here instead of a device, and the hub counts as connected. */
        void setSinkForTesting (Sink*);

        /** Index of the User port among these, or -1 (Push 3 before Push 2). */
        static int findPort (const juce::Array<juce::MidiDeviceInfo>&);

    private:
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override { dispatch (m); }
        void timerCallback() override;
        void tryOpen();

        const bool openDevices;
        mutable juce::CriticalSection lock;          // owner / attach vs the MIDI thread (never the audio thread)
        std::vector<PushController*> attached;
        PushController* owner = nullptr;
        std::unique_ptr<juce::MidiInput>  input;
        std::unique_ptr<juce::MidiOutput> output;
        juce::String portName;
        Sink* testSink = nullptr;
        std::atomic<uint32_t> connections { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PushHub)
    };

    /*
        PushController  (spec addition A7: Push 3 mode)
        --------------
        Turns Push 3 into a dedicated Twisted Breaks controller while it is in
        User mode (press Push's USER button; press it again to give Push back
        to Live).

          GRID   top 4 rows     the 32 pads: scene A left 4x4, scene B right 4x4,
                                laid out like the plugin's matrix (01 top left).
                                Hit = play (with velocity) + select.
                 bottom 4 rows  LANE layout: the selected pad's 32 steps, 8 per row.
                                Tap = on / off, hold + encoders = edit, hold one and
                                tap another = copy it across the range.
                                PADS layout (LAYOUT key): the scene on view as big
                                2-wide pads.
          PRESSURE              per pad (Push Expression: Poly Aftertouch), to the
                                pad's Pressure setting: filter, drive, stutter, pitch.
          ENCODERS 1-8          four pages (keys over the display / PAGE < >):
                                PAD  gain pan pitch start end slice character pressure
                                FX   lpf hpf crush drive tape stutter delay reverb
                                     (the keys under the display switch those effects
                                     on the selected pad's group)
                                SEQ  step, length, pad length, launch, mutate amount /
                                     auto / at, quantise
                                MIX  group A-D levels, crossfader, curve, macro amount,
                                     master
                                Holding steps: vel, chance, ratchet, pitch, slice,
                                micro, rev, condition of the held steps.
          MASTER encoder        master level.
          TOUCH STRIP           the A/B crossfader (while touched).
          BUTTONS               MUTE / SOLO + pad, DUPLICATE + pad (copy its lane to
                                the selected pad), DELETE + pad / step, SHIFT or
                                SELECT + pad = select only, < > / jog = previous /
                                next pad, up / down = scene A / B, FIXED LENGTH =
                                LEN 8 / 16 / 32 (SHIFT: the pad's own length),
                                UNDO = undo MUTATE, LAYOUT = lane / pads.
          SCENE COLUMN (top->)  MUTATE, FILL (hold), RESAMPLE, VIEW A/B, LAYOUT.
                                ACCENT = MUTATE and REPEAT = FILL too.

        Threads: the MIDI thread plays pads straight into the engine (its own
        lock-free queue) and writes pressure; everything else is queued to the
        message thread (timer, 50 Hz) which edits parameters and steps and
        sends only the lights that changed. Nothing new runs on the audio
        thread.
    */
    class PushController final : private juce::Timer
    {
    public:
        explicit PushController (TwistedBreaksProcessor&);
        ~PushController() override;

        // ---- Settings (message thread) ---------------------------------------------------
        void setEnabled (bool);
        bool isEnabled() const noexcept { return enabled; }
        /** This instance takes Push (you touched its window). */
        void claim();

        bool isConnected() const;
        bool isUserMode() const noexcept { return userMode.load(); }
        bool isOwner() const;
        bool isActive() const { return enabled && isConnected() && isOwner() && isUserMode(); }
        juce::String getPortName() const;

        std::function<void()> onSettingsChanged;
        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static const juce::Identifier pushId;

        // ---- What the plugin window shows --------------------------------------------------
        enum class Page { pad, fx, seq, mix };
        enum class Layout { lane, pads };
        Page   getPage() const noexcept { return page; }
        Layout getLayout() const noexcept { return layoutAtomic.load() == 0 ? Layout::lane : Layout::pads; }
        static const char* pageName (Page) noexcept;
        juce::String encoderLabel (int index) const;     // 0..7
        juce::String encoderValue (int index) const;
        /** Encoder touched or turned in the last 1.5 s, or -1. */
        int  getActiveEncoder() const noexcept;
        bool isEditingSteps() const noexcept { return ! heldSteps.empty(); }
        int  getSelectedPad() const noexcept { return selectedPad; }
        void setSelectedPad (int pad);
        uint32_t getSelectionVersion() const noexcept { return selectionVersion; }
        /** The last message from Push, e.g. "CC 58 = 127" (for checking the mapping). */
        juce::String getLastInput() const;

        // ---- MIDI thread (from the hub) ---------------------------------------------------------
        void handleIncoming (const juce::MidiMessage&);

        // ---- Testing ----------------------------------------------------------------------------------
        /** Uses this hub instead of the shared one, and enables. */
        void useHubForTesting (std::shared_ptr<PushHub>);
        void processPending (double nowMs);     // message-thread work (the timer does this)
        void tick (double nowMs);                // lights (the timer does this)
        /** The palette entry used for a colour right now. */
        int  paletteIndexFor (int colour) const noexcept { return colourIndex[(size_t) juce::jlimit (0, push::kNumColours - 1, colour)]; }
        /** Pad at a grid cell (row 0 = bottom), or -1 (a step cell). */
        int  padAt (int row, int col) const noexcept;

    private:
        struct Target
        {
            bool isStep = false;
            juce::String paramId;     // parameter
            int field = 0;            // step field
            juce::String label;
        };
        static constexpr int kNumStepFields = 8;

        void timerCallback() override;
        void attachHub();
        void detachHub();

        // MIDI thread
        void handleSysex (const juce::MidiMessage&);
        void enterUserMode();
        void leaveUserMode();
        void releaseHeldPlays();

        // Message thread
        void handleCell (int row, int col, int velocity, bool on, double nowMs);
        void handleButton (int cc, bool pressed, double nowMs);
        void turnEncoder (int index, int ticks, double nowMs);
        void touchEncoder (int index, bool touched, double nowMs);
        void endGesture (int index);
        void releaseEverything();
        std::array<Target, 8> targets() const;
        juce::RangedAudioParameter* param (const juce::String& id) const;
        float raw (const juce::String& id) const;
        void  setParamPlain (const juce::String& id, float plain);
        void  setPage (Page);
        void  selectPad (int pad, bool followScene = true);
        int   laneLength (int pad) const;
        int   groupOf (int pad) const;
        void  editStepField (int field, int units);
        void  pressStep (int step, int velocity);
        void  releaseStep (int step);
        void  cycleLength (bool padOwn);
        void  setFill (bool on);
        void  toggleFx (int slot, bool pressed);
        static int fxForSlot (int slot) noexcept;

        // Lights
        int  padColour (int pad, bool blinkOn) const;
        int  stepColour (int step, int laneLen, int playhead) const;
        void sendPad (int note, int colour, bool force);
        void sendButton (int cc, int value, bool force);
        void allOff();
        void requestPalette (double nowMs);

        TwistedBreaksProcessor& processor;
        EngineBridge& bridge;
        juce::AudioProcessorValueTreeState& apvts;
        struct PadRaw { std::atomic<float>* mute; std::atomic<float>* mode; std::atomic<float>* seqLen; std::atomic<float>* group; std::atomic<float>* solo; };
        std::array<PadRaw, kNumPads> padRaw {};
        std::atomic<float>* sceneViewRaw = nullptr;
        std::atomic<float>* seqLengthRaw = nullptr;

        std::shared_ptr<PushHub> hub;
        bool enabled = false;
        bool testing = false;

        // ---- Shared between the MIDI thread and the message thread (atomics / SPSC) --------------
        struct RawIn { uint8_t b0 = 0, b1 = 0, b2 = 0; };
        SpscQueue<RawIn, 1024> inbox;                      // notes + button CCs, in order
        std::array<std::atomic<int>, 9> encoderTicks {};   // 0..7, 8 = master
        std::atomic<int> jogTicks { 0 };
        std::atomic<bool> userMode { false }, sawModeSysex { false };
        std::atomic<bool> needRedraw { false }, needRelease { false };
        std::atomic<uint32_t> midiMods { 0 };              // modifier keys held (MIDI thread's view)
        std::atomic<int> layoutAtomic { 0 };               // 0 lane, 1 pads
        std::atomic<int> lastInput { -1 };                 // packed status / data1 / data2
        std::atomic<int> sysexDev { 0x01 }, sysexModel { 0x01 };
        std::array<std::atomic<int>, 128> paletteRgb {};    // 0xRRGGBB, -1 unknown
        std::atomic<uint32_t> paletteReplies { 0 };
        std::array<int, 128> heldPlay {};                    // MIDI thread only: note -> pad playing, -1
        int lastPressedPad = -1;                             // MIDI thread only

        // ---- Message thread -------------------------------------------------------------------------
        Page page = Page::pad;
        int  selectedPad = 0;
        uint32_t selectionVersion = 0;
        std::bitset<128> held;                               // buttons held (in inbox order)
        struct HeldStep { int step; bool created; bool edited; };
        std::vector<HeldStep> heldSteps;
        std::array<int, 9> encoderAcc {};
        std::array<bool, 9> encoderTouched {};
        std::array<juce::RangedAudioParameter*, 9> gestureParam {};
        std::array<double, 9> lastTurnMs {};
        int activeEncoder = -1;
        double activeEncoderMs = -1.0e9;
        bool stripTouched = false;
        juce::RangedAudioParameter* stripParam = nullptr;
        bool fillHeld = false;
        std::array<juce::String, 8> fxHeld;                  // momentary FX a lower key switched on (to release)

        std::array<int, push::kNumColours> colourIndex {};
        uint32_t paletteSeen = 0, connectionSeen = 0;
        int paletteNext = 128;                               // next entry to request
        double paletteRequestedMs = -1.0e9;
        int paletteDev = -1, paletteModel = -1;
        std::array<int, 64>  sentPad {};
        std::array<int, 128> sentButton {};
        bool wasActive = false;
        double lastFullRefreshMs = -1.0e9;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PushController)
    };
}
