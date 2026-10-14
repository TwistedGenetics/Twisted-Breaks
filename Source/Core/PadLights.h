#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <array>
#include <functional>
#include "Core/EngineBridge.h"

namespace tb
{
    class AudioEngine;

    /*
        PadLights
        ---------
        Lights the pads of an IK Multimedia iRig Pads (or any controller using
        the same scheme) to show what Twisted Breaks is doing:

            green, brightness following the sound   pad is playing
            dim green                               pad has a sample, idle
            flashing red                            SWAP queued onto this pad
            off                                     empty pad

        iRig Pads lights a pad from two Control Changes on MIDI channel 1, one
        per LED colour, value 0..127 = brightness (manual, section 4). Hardware
        pad k (1..16) uses this pair:

            pad  1  94/95    pad  5  86/87    pad  9  78/79    pad 13  70/71
            pad  2  96/97    pad  6  88/89    pad 10  80/81    pad 14  72/73
            pad  3  98/99    pad  7  90/91    pad 11  82/83    pad 15  74/75
            pad  4 100/101   pad  8  92/93    pad 12  84/85    pad 16  76/77

        The manual lists each pair as RED-GREEN; SWAP COLOURS flips it in case
        a unit is the other way round.

        Which hardware pad plays which plugin pad comes from the notes: with
        the iRig on its default scene (General MIDI kit) each hardware pad's
        note is known, and each plugin pad's note is in the note map. For any
        other scene, LEARN lights the hardware pads in turn; hitting each lit
        pad records where it is.

        The CCs go straight to the device's MIDI output (CoreMIDI on the Mac),
        not through Ableton's routing, from a 30 Hz timer thread — never from
        the audio thread. Only changes are sent, plus a full refresh every few
        seconds and whenever the device (re)appears, so unplugging and
        replugging just works. Everything is turned off on close.
    */
    class PadLights final : private juce::HighResolutionTimer,
                            private juce::AsyncUpdater
    {
    public:
        /** Where light messages go (a MIDI output, or a recorder in tests). */
        struct Sink
        {
            virtual ~Sink() = default;
            virtual void send (const juce::MidiMessage&) = 0;
        };

        PadLights (EngineBridge&, AudioEngine&);
        ~PadLights() override;

        // ---- Settings (message thread) -----------------------------------------
        /** MIDI output name to light ("" = off). Opens it, retrying if it isn't plugged in. */
        void setDevice (const juce::String& deviceName);
        juce::String getDevice() const;
        bool isConnected() const;

        void setSwapColours (bool);
        bool getSwapColours() const;

        void startLearn();
        void stopLearn();
        bool isLearning() const;
        int  getLearnStep() const;            // 0..15 while learning
        bool hasLearnedLayout() const;
        void forgetLearnedLayout();

        void runTest();                       // ~2.5 s: all red, all green, both, off

        /** Fires (message thread) when a setting changes, so the host saves it. */
        std::function<void()> onSettingsChanged;

        juce::ValueTree toValueTree() const;
        void fromValueTree (const juce::ValueTree&);
        static const juce::Identifier lightsId;

        /** MIDI outputs on this machine, iRig Pads first. */
        static juce::StringArray availableDevices();

        // ---- Mapping ------------------------------------------------------------
        /** Hardware pad (0..15) lighting `pluginPad`, or -1. */
        int hardwarePadFor (int pluginPad) const;
        /** Note an iRig Pads hardware pad sends on its default scene. */
        static int defaultSceneNote (int hardwarePad) noexcept;
        /** CC for hardware pad (0..15) and colour. */
        static int ccFor (int hardwarePad, bool red, bool swapColours) noexcept;

        // ---- Testing --------------------------------------------------------------
        void setSinkForTesting (Sink*);
        /** One frame of the light show at time `nowMs`. */
        void tick (double nowMs);

    private:
        void hiResTimerCallback() override;
        void handleAsyncUpdate() override;
        void updateTimer();
        void tryOpen (double nowMs);
        void sendAllOff();
        void sendLed (int hardwarePad, int red, int green, bool force);

        EngineBridge& bridge;
        AudioEngine&  engine;

        mutable juce::CriticalSection lock;     // settings vs the timer thread (never the audio thread)
        juce::String deviceName;
        std::unique_ptr<juce::MidiOutput> output;
        Sink* testSink = nullptr;
        bool swapColours = false;

        std::array<int, kPadsPerScene> learned {};   // plugin pad -> hardware pad, -1 = not learned
        bool learning = false;
        int  learnStep = 0;
        uint32_t learnMidiCount = 0;

        double testStartMs = -1.0;
        double lastOpenAttemptMs = -1.0e9, lastFullRefreshMs = -1.0e9;

        std::array<int, kPadsPerScene> sentRed {}, sentGreen {};   // last values sent, -1 = unknown

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadLights)
    };
}
