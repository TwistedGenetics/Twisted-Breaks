#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "UI/PadGrid.h"
#include "UI/TransportDisplay.h"
#include "UI/ScopeDisplay.h"
#include "UI/NeedleMeter.h"
#include "UI/FXGrid.h"

class TwistedBreaksProcessor;

namespace tb::ui
{
    //==========================================================================
    /** Bat-handle toggle switch on a ВКЛ / ОТКЛ (on/off) plate. */
    class ToggleSwitch final : public juce::Button
    {
    public:
        ToggleSwitch();
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    /** The AZ-5 style emergency button: red mushroom in a hazard collar. */
    class PanicButton final : public juce::Button
    {
    public:
        PanicButton();
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void flash() { flashFrames = 12; repaint(); }
        void tick() { if (flashFrames > 0) { --flashFrames; repaint(); } }
    private:
        int flashFrames = 0;
    };

    /** Section selector key in the top rail. Only PADS exists in Phase 1. */
    class SelectorKey final : public juce::Button
    {
    public:
        SelectorKey (const juce::String& latin, const juce::String& cyrillic, int phase);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        const int phase;
    private:
        juce::String latinName, cyrName;
    };

    /** MIDI input lamp, last note received and the selected pad's mapping. */
    class StatusStrip final : public juce::Component
    {
    public:
        explicit StatusStrip (TwistedBreaksProcessor&);
        void refresh (int selectedPad);
        void paint (juce::Graphics&) override;
    private:
        TwistedBreaksProcessor& processor;
        uint32_t lastCount = 0;
        float    midiLamp  = 0.0f;
        int      lastNote  = -1, lastVel = 0, selPad = 0, selNote = 36;
    };

    //==========================================================================
    /*
        MainView
        --------
        The whole control console at a fixed logical size (1200 x 800). The
        editor scales it as a unit, so layout code never deals with scaling.

        Static artwork (enamel, rust, rivets, plates, captions) is rendered
        once into `background`; only lamps, displays and meters repaint on the
        30 Hz timer, which keeps CPU use low inside Ableton.
    */
    class MainView final : public juce::Component,
                           private juce::Timer
    {
    public:
        static constexpr int kWidth  = 1200;
        static constexpr int kHeight = 800;

        explicit MainView (TwistedBreaksProcessor&);
        ~MainView() override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void rebuildBackground();

        TwistedBreaksProcessor& processor;

        PadGrid          padGrid;
        TransportDisplay transportDisplay;
        ScopeDisplay     scope;
        NeedleMeter      meter;
        FXGrid           fxGrid;
        StatusStrip      status;

        juce::Slider  masterKnob;
        ToggleSwitch  limiterSwitch;
        PanicButton   panicButton;
        juce::OwnedArray<SelectorKey> navKeys;

        using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
        using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
        std::unique_ptr<SliderAttachment> masterAttachment;
        std::unique_ptr<ButtonAttachment> limiterAttachment;

        juce::Image background;
        juce::TooltipWindow tooltips { this, 600 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainView)
    };
}
